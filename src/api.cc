#include "aktualizr-lite/api.h"

#include <sys/file.h>
#include <unistd.h>
#include <boost/process.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "liteclient.h"
#include "primary/reportqueue.h"

#include "aktualizr-lite/tuf/tuf.h"
#include "composeapp/appengine.h"
#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "ostree/repo.h"
#include "tuf/akhttpsreposource.h"
#include "tuf/akrepo.h"
#include "tuf/localreposource.h"
#include "uptane/exceptions.h"

const std::vector<boost::filesystem::path> AkliteClient::CONFIG_DIRS = {"/usr/lib/sota/conf.d", "/var/sota/sota.toml",
                                                                        "/etc/sota/conf.d/"};

static std::pair<CheckInResult::Status, std::string> checkAndGetBundleTargets(
    const std::shared_ptr<aklite::tuf::Repo>& tuf_repo, const boost::filesystem::path& bundle_tuf_path,
    std::vector<TufTarget>& out_bundle_targets);

// A key for json sub-document containing paths to update content (ostree repo and apps store).
// It is applicable only to offline/local update.
const static std::string LocalSrcDirKey{"local-src-dir"};

TufTarget CheckInResult::GetLatest(std::string hwid) const {
  if (hwid.empty()) {
    hwid = primary_hwid_;
  }

  for (auto it = targets_.crbegin(); it != targets_.crend(); ++it) {
    if ((*it).Custom()["hardwareIds"][0] == hwid) {
      return *it;
    }
  }
  throw std::runtime_error("no target for this hwid");
}

std::ostream& operator<<(std::ostream& os, const DownloadResult& res) {
  if (res.status == DownloadResult::Status::Ok) {
    os << "Ok/";
  } else if (res.status == DownloadResult::Status::DownloadFailed) {
    os << "DownloadFailed/";
  } else if (res.status == DownloadResult::Status::VerificationFailed) {
    os << "VerificationFailed/";
  } else if (res.status == DownloadResult::Status::DownloadFailed_NoSpace) {
    os << "DownloadFailed_NoSpace/";
  }
  os << res.description;
  return os;
}

std::ostream& operator<<(std::ostream& os, const InstallResult& res) {
  if (res.status == InstallResult::Status::Ok) {
    os << "Ok/";
  } else if (res.status == InstallResult::Status::OkBootFwNeedsCompletion) {
    os << "OkBootFwNeedsCompletion/";
  } else if (res.status == InstallResult::Status::NeedsCompletion) {
    os << "NeedsCompletion/";
  } else if (res.status == InstallResult::Status::BootFwNeedsCompletion) {
    os << "BootFwNeedsCompletion/";
  } else if (res.status == InstallResult::Status::Failed) {
    os << "Failed/";
  } else if (res.status == InstallResult::Status::DownloadFailed) {
    os << "DownloadFailed/";
  }
  os << res.description;
  return os;
}

static void assert_lock() {
  // Leave this open for the remainder of the process to keep the lock held
  int fd = open("/var/lock/aklite.lock", O_CREAT | O_RDONLY, 0444);
  if (fd == -1) {
    throw std::system_error(errno, std::system_category(), "An error occurred opening the aklite lock");
  }

  if (flock(fd, LOCK_NB | LOCK_EX) == -1) {
    if (errno == EWOULDBLOCK) {
      throw std::runtime_error("Failed to obtain the aklite lock, another instance must be running !!!");
    }
    throw std::system_error(errno, std::system_category(), "An error occurred obtaining the aklite lock");
  }
}

void AkliteClient::Init(Config& config, bool finalize, bool apply_lock) {
  if (!read_only_) {
    if (apply_lock) {
      assert_lock();
    }
    config.telemetry.report_network = !config.tls.server.empty();
    config.telemetry.report_config = !config.tls.server.empty();
  }
  if (client_ == nullptr) {
    client_ = std::make_unique<LiteClient>(config, nullptr);
  }
  if (!read_only_) {
    client_->importRootMetaIfNeededAndPresent();
    if (finalize) {
      client_->finalizeInstall();
    }
  }

  tuf_repo_ = std::make_unique<aklite::tuf::AkRepo>(client_->config);
  hw_id_ = client_->primary_ecu.second.ToString();
}

AkliteClient::AkliteClient(const std::vector<boost::filesystem::path>& config_dirs, bool read_only, bool finalize) {
  read_only_ = read_only;
  Config config(config_dirs);
  Init(config, finalize);
}

AkliteClient::AkliteClient(const boost::program_options::variables_map& cmdline_args, bool read_only, bool finalize) {
  read_only_ = read_only;
  Config config(cmdline_args);
  Init(config, finalize);
}

AkliteClient::AkliteClient(std::shared_ptr<LiteClient> client, bool read_only, bool apply_lock)
    : read_only_{read_only}, client_(std::move(client)) {
  Init(client_->config, false, apply_lock);
}

AkliteClient::~AkliteClient() {
  // Release the lock to allow reobtaining with another instance.
  unlink("/var/lock/aklite.lock");
}

static bool compareTargets(const TufTarget& a, const TufTarget& b) { return a.Version() < b.Version(); }

// Returns a sorted list of targets matching tags if configured and hwid (or one of secondary_hwids)
static std::vector<TufTarget> filterTargets(const std::vector<TufTarget>& allTargets, const std::string& hwidToFind,
                                            const std::vector<std::string>& tags,
                                            const std::vector<std::string>& secondary_hwids) {
  std::vector<TufTarget> targets;
  for (const auto& t : allTargets) {
    if (!tags.empty() /* Should we really allow a device configuration without a tag? */
        && !t.HasOneOfTags(tags)) {
      continue;
    }
    if (t.HardwareId() == hwidToFind) {
      targets.push_back(t);
      continue;
    }
    for (const auto& hwid : secondary_hwids) {
      if (t.HardwareId() == hwid) {
        targets.push_back(t);
        break;
      }
    }
  }
  std::sort(targets.begin(), targets.end(), compareTargets);
  return targets;
}

CheckInResult AkliteClient::CheckIn() const {
  client_->notifyTufUpdateStarted();
  if (!configUploaded_) {
    client_->reportAktualizrConfiguration();
    configUploaded_ = true;
  }
  client_->reportNetworkInfo();
  client_->reportHwInfo();
  client_->reportAppsState();

  boost::property_tree::ptree pt;
  pt.put<std::string>("tag", client_->tags.empty() ? "" : client_->tags.at(0));
  auto current = client_->getCurrent();
  pt.put<std::string>("dockerapps", Target::appsStr(current, ComposeAppManager::Config(client_->config.pacman).apps));
  pt.put<std::string>("target", current.filename());
  pt.put<std::string>("ostreehash", current.sha256Hash());

  LOG_INFO << "Updating the local TUF repo with metadata located in " << client_->config.uptane.repo_server << "...";
  auto repo_src = std::make_shared<aklite::tuf::AkHttpsRepoSource>("temp-remote-repo-source", pt, client_->config);
  CheckInResult::Status check_status{CheckInResult::Status::Failed};
  std::string err_msg;
  try {
    tuf_repo_->UpdateMeta(repo_src);
    check_status = CheckInResult::Status::Ok;
    LOG_INFO << "The local TUF repo has been successfully updated";
  } catch (const Uptane::SecurityException& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo; TUF metadata check failure: " + err;
    check_status = CheckInResult::Status::SecurityError;
  } catch (const Uptane::ExpiredMetadata& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo; TUF metadata is expired: " + err;
    check_status = CheckInResult::Status::ExpiredMetadata;
  } catch (const Uptane::MetadataFetchFailure& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo; TUF metadata fetch failure: " + err;
    if (err.find("Failed to fetch role timestamp") != std::string::npos) {
      err_msg += "Check the device tag or verify the existence of a wave for the device";
    }
    check_status = CheckInResult::Status::MetadataFetchFailure;
  } catch (const std::exception& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo: " + err;
    check_status = CheckInResult::Status::Failed;
  }
  if (check_status != CheckInResult::Status::Ok) {
    LOG_INFO << "Checking the local TUF repo...";
    try {
      tuf_repo_->CheckMeta();
      check_status = CheckInResult::Status::OkCached;
      LOG_INFO << "The local TUF is valid";
    } catch (const std::exception& exc) {
      err_msg = std::string("Local TUF repo is invalid: ") + exc.what();
    }
  }
  if (!err_msg.empty()) {
    LOG_ERROR << err_msg;
  }

  if (check_status != CheckInResult::Status::Ok && check_status != CheckInResult::Status::OkCached) {
    client_->notifyTufUpdateFinished(err_msg);
    return CheckInResult{check_status, "", {}};
  }

  LOG_INFO << "Searching for matching TUF Targets...";
  auto matchingTargets = filterTargets(tuf_repo_->GetTargets(), hw_id_, client_->tags, secondary_hwids_);
  if (matchingTargets.empty()) {
    err_msg =
        "Couldn't find Targets matching the current device tag and hardware ID; check a device tag or a hardware ID";
    LOG_ERROR << err_msg;
    client_->notifyTufUpdateFinished(err_msg);
    return CheckInResult{CheckInResult::Status::NoMatchingTargets, "", {}};
  }
  LOG_INFO << "Found " << matchingTargets.size() << " matching TUF Targets";
  client_->notifyTufUpdateFinished();
  return CheckInResult(CheckInResult::Status::Ok, hw_id_, matchingTargets);
}

static bool compareTargets(const Uptane::Target& t1, const Uptane::Target& t2) {
  return !(Target::Version(t1.custom_version()) < Target::Version(t2.custom_version()));
}

struct UpdateSrc {
  boost::filesystem::path TufDir;
  boost::filesystem::path OstreeRepoDir;
  boost::filesystem::path AppsDir;
  std::string TargetName;
};

static void parseUpdateContent(const boost::filesystem::path& apps_dir, std::set<std::string>& found_apps) {
  if (!boost::filesystem::exists(apps_dir)) {
    return;
  }
  for (auto const& app_dir_entry : boost::filesystem::directory_iterator{apps_dir}) {
    const auto app_name{app_dir_entry.path().filename().string()};
    for (auto const& app_ver_dir_entry : boost::filesystem::directory_iterator{app_dir_entry.path()}) {
      const auto uri_file{app_ver_dir_entry.path() / "uri"};
      const auto app_uri{Utils::readFile(uri_file.string())};
      LOG_DEBUG << "Found app; uri: " << app_uri;
      found_apps.insert(app_uri);
    }
  }
}

static std::vector<Uptane::Target> getAvailableTargets(const PackageConfig& pconfig,
                                                       const std::vector<Uptane::Target>& allowed_targets,
                                                       const UpdateSrc& src, bool just_latest = true) {
  if (allowed_targets.empty()) {
    LOG_ERROR << "No targets are available for a given device; check a hardware ID and/or a tag";
    return std::vector<Uptane::Target>{};
  }
  std::vector<Uptane::Target> found_targets;
  std::set<std::string> found_apps;

  parseUpdateContent(src.AppsDir / "apps", found_apps);
  LOG_INFO << "Apps found in the source directory " << src.AppsDir;
  for (const auto& app : found_apps) {
    LOG_INFO << "\t" << app;
  }

  const OSTree::Repo repo{src.OstreeRepoDir.string()};
  Uptane::Target found_target(Uptane::Target::Unknown());

  const std::string search_msg{just_latest ? "a target" : "all targets"};
  LOG_INFO << "Searching for " << search_msg << " starting from " << allowed_targets.begin()->filename()
           << " that match content provided in the source directory\n"
           << "\tpacman type: \t" << pconfig.type << "\n\t apps dir: \t" << src.AppsDir << "\n\t ostree dir: \t"
           << src.OstreeRepoDir;
  for (const auto& t : allowed_targets) {
    LOG_INFO << "Checking " << t.filename();
    if (!repo.hasCommit(t.sha256Hash())) {
      LOG_INFO << "\tmissing ostree commit: " << t.sha256Hash();
      continue;
    }
    if (pconfig.type != ComposeAppManager::Name) {
      auto custom_data{t.custom_data()};
      custom_data[LocalSrcDirKey]["ostree"] = src.OstreeRepoDir.string();
      found_targets.emplace_back(Target::updateCustom(t, custom_data));
      LOG_INFO << "\tall target content have been found";
      if (!just_latest) {
        continue;
      }
      break;
    }
    const ComposeAppManager::AppsContainer required_apps{
        ComposeAppManager::getRequiredApps(ComposeAppManager::Config(pconfig), t)};
    std::set<std::string> missing_apps;

    std::set<std::string> offline_app_shortlist;
    const auto custom{t.custom_data()};
    if (custom.isMember("fetched-apps") && custom["fetched-apps"].isObject() &&
        custom["fetched-apps"].isMember("shortlist")) {
      boost::split(offline_app_shortlist, custom["fetched-apps"]["shortlist"].asString(), boost::is_any_of(", "),
                   boost::token_compress_on);
    }
    for (const auto& app : required_apps) {
      if (found_apps.count(app.second) == 0 &&
          // If the app shortlist is ON and the missing app is not in the app shortlist, then
          // don't treat it as a missing app.
          (offline_app_shortlist.empty() || offline_app_shortlist.count(app.first) == 1)) {
        missing_apps.insert(app.second);
      }
    }
    if (!missing_apps.empty()) {
      LOG_INFO << "\tmissing apps:";
      for (const auto& app : missing_apps) {
        LOG_INFO << "\t\t" << app;
      }
      continue;
    }
    auto custom_data{t.custom_data()};
    if (!offline_app_shortlist.empty()) {
      // If the app shortlist is ON then update the found target app list so the downloader
      // and the installer will update only the apps specified in the shortlist.
      Json::Value app_shortlist;
      for (const auto& app : offline_app_shortlist) {
        app_shortlist[app] = custom_data["docker_compose_apps"][app];
      }
      custom_data["docker_compose_apps"] = app_shortlist;
    }
    custom_data[LocalSrcDirKey]["ostree"] = src.OstreeRepoDir.string();
    custom_data[LocalSrcDirKey]["apps"] = src.AppsDir.string();
    found_targets.emplace_back(Target::updateCustom(t, custom_data));
    LOG_INFO << "\tall target content have been found";
    if (just_latest) {
      break;
    }
  }
  return found_targets;
}

std::vector<Uptane::Target> fromTufTargets(const std::vector<TufTarget>& tufTargets) {
  std::vector<Uptane::Target> ret;
  ret.reserve(tufTargets.size());
  for (auto const& t : tufTargets) {
    ret.emplace_back(Target::fromTufTarget(t));
  }
  return ret;
}

std::vector<TufTarget> toTufTargets(const std::vector<Uptane::Target>& targets) {
  std::vector<TufTarget> ret;
  ret.reserve(targets.size());
  for (auto const& t : targets) {
    ret.emplace_back(Target::toTufTarget(t));
  }
  return ret;
}

CheckInResult AkliteClient::CheckInLocal(const LocalUpdateSource* local_update_source) const {
  client_->notifyTufUpdateStarted();
  auto repo_src =
      std::make_shared<aklite::tuf::LocalRepoSource>("temp-local-repo-source", local_update_source->tuf_repo);

  LOG_INFO << "Updating the local TUF repo with metadata located in " << local_update_source->tuf_repo << "...";
  std::string err_msg;
  CheckInResult::Status check_status{CheckInResult::Status::Ok};
  try {
    tuf_repo_->UpdateMeta(repo_src);
    LOG_INFO << "The local TUF repo has been successfully updated";
  } catch (const Uptane::SecurityException& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo; TUF metadata check failure: " + err;
    check_status = CheckInResult::Status::SecurityError;
  } catch (const Uptane::ExpiredMetadata& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo; TUF metadata is expired: " + err;
    check_status = CheckInResult::Status::ExpiredMetadata;
  } catch (const aklite::tuf::MetadataNotFoundException& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo; TUF metadata not found: " + err;
    check_status = CheckInResult::Status::MetadataNotFound;
  } catch (const std::exception& exc) {
    const std::string err{exc.what()};
    err_msg = "Failed to update the local TUF repo: " + err;
    check_status = CheckInResult::Status::Failed;
  }

  if (check_status != CheckInResult::Status::Ok) {
    LOG_ERROR << err_msg;
    client_->notifyTufUpdateFinished(err_msg);
    return CheckInResult{check_status, "", {}};
  }

  LOG_INFO << "The local TUF repo has been successfully updated";
  LOG_INFO << "Checking the bundle metadata...";
  std::vector<TufTarget> bundle_targets;
  const auto bundle_targets_check{checkAndGetBundleTargets(tuf_repo_, local_update_source->tuf_repo, bundle_targets)};
  if (bundle_targets_check.first != CheckInResult::Status::Ok) {
    LOG_ERROR << bundle_targets_check.second;
    client_->notifyTufUpdateFinished(bundle_targets_check.second);
    return CheckInResult{bundle_targets_check.first, "", {}};
  }

  LOG_INFO << "Searching for TUF Targets matching a device's tag and hardware ID...";
  auto matchingTargets = filterTargets(bundle_targets, hw_id_, client_->tags, secondary_hwids_);
  if (matchingTargets.empty()) {
    err_msg =
        "Couldn't find Targets matching the current device tag and hardware ID; check a device tag or a hardware ID";
    LOG_ERROR << err_msg;
    return CheckInResult{CheckInResult::Status::NoMatchingTargets, "", {}};
  }
  LOG_INFO << "Found " << matchingTargets.size() << " matching TUF Targets";

  LOG_INFO << "Looking for update content for each matching Target...";
  UpdateSrc src{
      .TufDir = local_update_source->tuf_repo,
      .OstreeRepoDir = local_update_source->ostree_repo,
      .AppsDir = local_update_source->app_store,
  };
  std::vector<Uptane::Target> available_targets =
      getAvailableTargets(client_->config.pacman, fromTufTargets(matchingTargets), src,
                          false /* get all available targets, not just latest */);
  if (available_targets.empty()) {
    err_msg =
        "No update content found in ostree dir  " + src.OstreeRepoDir.string() + " and app dir " + src.AppsDir.string();
    LOG_ERROR << err_msg;
    client_->notifyTufUpdateFinished(err_msg);
    return CheckInResult(CheckInResult::Status::NoTargetContent, hw_id_, std::vector<TufTarget>{});
  }

  client_->notifyTufUpdateFinished();
  return CheckInResult(CheckInResult::Status::Ok, hw_id_, toTufTargets(available_targets));
}

boost::property_tree::ptree AkliteClient::GetConfig() const {
  std::stringstream ss;
  ss << client_->config;

  boost::property_tree::ptree pt;
  boost::property_tree::ini_parser::read_ini(ss, pt);
  return pt;
}

TufTarget AkliteClient::GetCurrent() const { return Target::toTufTarget(client_->getCurrent()); }

DeviceResult AkliteClient::GetDevice() const {
  DeviceResult res{DeviceResult::Status::Failed};
  const auto http_res = client_->http_client->get(client_->config.tls.server + "/device", HttpInterface::kNoLimit);
  if (http_res.isOk()) {
    const Json::Value device_info = http_res.getJson();
    if (!device_info.empty()) {
      res.status = DeviceResult::Status::Ok;
      res.name = device_info["Name"].asString();
      res.factory = device_info["factory"].asString();
      res.owner = device_info["owner"].asString();
      res.repo_id = device_info["repo_id"].asString();
    } else {
      LOG_WARNING << "Failed to get a device name from a device info: " << device_info;
    }
  }
  return res;
}

std::string AkliteClient::GetDeviceID() const { return client_->getDeviceID(); }

class LiteInstall : public InstallContext {
 public:
  LiteInstall(std::shared_ptr<LiteClient> client, std::unique_ptr<Uptane::Target> t, std::string& reason,
              InstallMode install_mode = InstallMode::All)
      : client_(std::move(client)), target_(std::move(t)), reason_(reason), mode_{install_mode} {}

  InstallResult Install() override {
    client_->logTarget("Installing: ", *target_);

    // Call appsInSync to update applications list inside the package manager
    client_->appsInSync(*target_);
    // setAppsNotChecked is required to force a re-load of apps list in case of a new Download operation
    client_->setAppsNotChecked();
    if (client_->VerifyTarget(*target_) != TargetStatus::kGood) {
      return InstallResult{InstallResult::Status::DownloadFailed, ""};
    }

    auto rc = client_->install(*target_, mode_);
    auto status = InstallResult::Status::Failed;
    if (rc == data::ResultCode::Numeric::kNeedCompletion) {
      if (client_->isPendingTarget(*target_)) {
        if (client_->getCurrent().sha256Hash() == target_->sha256Hash()) {
          status = InstallResult::Status::AppsNeedCompletion;
        } else {
          status = InstallResult::Status::NeedsCompletion;
        }
      } else {
        // If the install returns `kNeedCompletion` and the target being installed is not pending,
        // then it means that the previous boot fw update requires reboot prior to running the new target update
        status = InstallResult::Status::BootFwNeedsCompletion;
      }
    } else if (rc == data::ResultCode::Numeric::kOk) {
      status = InstallResult::Status::Ok;
    } else if (rc == data::ResultCode::Numeric::kDownloadFailed) {
      status = InstallResult::Status::DownloadFailed;
    }
    return InstallResult{status, ""};
  }

  DownloadResult Download() override {
    auto reason = reason_;
    if (reason.empty()) {
      reason = "Update to " + target_->filename();
    }

    client_->logTarget("Downloading: ", *target_);

    auto download_res{client_->download(*target_, reason)};
    if (!download_res) {
      return DownloadResult{download_res.status, download_res.description, download_res.destination_path};
    }

    if (client_->VerifyTarget(*target_) != TargetStatus::kGood) {
      data::InstallationResult ires{data::ResultCode::Numeric::kVerificationFailed, "Downloaded target is invalid"};
      client_->notifyInstallFinished(*target_, ires);
      return DownloadResult{DownloadResult::Status::VerificationFailed, ires.description};
    }

    return DownloadResult{DownloadResult::Status::Ok, ""};
  }

  std::string GetCorrelationId() override { return target_->correlation_id(); }

  void QueueEvent(std::string ecu_serial, SecondaryEvent event, std::string details) override {
    Uptane::EcuSerial serial(ecu_serial);
    std::unique_ptr<ReportEvent> e;
    if (event == InstallContext::SecondaryEvent::DownloadStarted) {
      e = std::make_unique<EcuDownloadStartedReport>(serial, target_->correlation_id());
    } else if (event == InstallContext::SecondaryEvent::DownloadCompleted) {
      e = std::make_unique<EcuDownloadCompletedReport>(serial, target_->correlation_id(), true);
    } else if (event == InstallContext::SecondaryEvent::DownloadFailed) {
      e = std::make_unique<EcuDownloadCompletedReport>(serial, target_->correlation_id(), false);
    } else if (event == InstallContext::SecondaryEvent::InstallStarted) {
      e = std::make_unique<EcuInstallationStartedReport>(serial, target_->correlation_id());
    } else if (event == InstallContext::SecondaryEvent::InstallCompleted) {
      e = std::make_unique<EcuInstallationCompletedReport>(serial, target_->correlation_id(), true);
    } else if (event == InstallContext::SecondaryEvent::InstallFailed) {
      e = std::make_unique<EcuInstallationCompletedReport>(serial, target_->correlation_id(), false);
    } else if (event == InstallContext::SecondaryEvent::InstallNeedsCompletion) {
      e = std::make_unique<EcuInstallationAppliedReport>(serial, target_->correlation_id());
    } else {
      throw std::runtime_error("Invalid secondary event");
    }

    if (!details.empty()) {
      e->custom["details"] = details;
    }

    e->custom["targetName"] = target_->filename();
    e->custom["version"] = target_->custom_version();
    client_->report_queue->enqueue(std::move(e));
  }

 protected:
  std::shared_ptr<LiteClient> client_;
  std::unique_ptr<Uptane::Target> target_;
  std::string reason_;
  InstallMode mode_;
};

class BaseHttpClient : public HttpInterface {
 public:
  HttpResponse post(const std::string& url, const std::string& content_type, const std::string& data) override {
    (void)url;
    (void)content_type;
    (void)data;
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse post(const std::string& url, const Json::Value& data) override {
    (void)url;
    (void)data;
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse put(const std::string& url, const std::string& content_type, const std::string& data) override {
    (void)url;
    (void)content_type;
    (void)data;
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse put(const std::string& url, const Json::Value& data) override {
    (void)url;
    (void)data;
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse download(const std::string& url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void* userp, curl_off_t from) override {
    (void)url;
    (void)write_cb;
    (void)progress_cb;
    (void)userp;
    (void)from;
    return HttpResponse("", 501, CURLE_OK, "");
  }
  std::future<HttpResponse> downloadAsync(const std::string& url, curl_write_callback write_cb,
                                          curl_xferinfo_callback progress_cb, void* userp, curl_off_t from,
                                          CurlHandler* easyp) override {
    (void)url;
    (void)write_cb;
    (void)progress_cb;
    (void)userp;
    (void)from;
    (void)easyp;
    std::promise<HttpResponse> resp_promise;
    resp_promise.set_value(HttpResponse("", 501, CURLE_OK, ""));
    return resp_promise.get_future();
  }
  void setCerts(const std::string& ca, CryptoSource ca_source, const std::string& cert, CryptoSource cert_source,
                const std::string& pkey, CryptoSource pkey_source) override {
    (void)ca;
    (void)ca_source;
    (void)cert;
    (void)cert_source;
    (void)pkey;
    (void)pkey_source;
  }
};

class RegistryBasicAuthClient : public BaseHttpClient {
 public:
  HttpResponse get(const std::string& url, int64_t maxsize) override {
    (void)url;
    (void)maxsize;
    return HttpResponse(R"({"Secret":"secret","Username":"test-user"})", 200, CURLE_OK, "");
  }
};

class OfflineRegistry : public BaseHttpClient {
 public:
  explicit OfflineRegistry(boost::filesystem::path root_dir, std::string hostname = "hub.foundries.io")
      : hostname_{std::move(hostname)}, root_dir_{std::move(root_dir)} {}

  HttpResponse get(const std::string& url, int64_t maxsize) override {
    (void)maxsize;
    if (boost::starts_with(url, auth_endpoint_)) {
      return HttpResponse(R"({"token":"token"})", 200, CURLE_OK, "");
    }
    return getAppItem(url);
  }

  HttpResponse download(const std::string& url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void* userp, curl_off_t from) override {
    (void)progress_cb;
    (void)from;
    const std::string hash_prefix{"sha256:"};
    const auto digest_pos{url.rfind(hash_prefix)};
    if (digest_pos == std::string::npos) {
      return HttpResponse("Invalid URL", 400, CURLE_OK, "");
    }
    const auto hash_pos{digest_pos + hash_prefix.size()};
    const auto hash{url.substr(hash_pos)};
    const auto blob_path{(blobs_dir_ / hash).string()};

    if (!boost::filesystem::exists(blob_path)) {
      return HttpResponse("The app blob is missing: " + blob_path, 404, CURLE_OK, "Not found");
    }

    std::array<char, 1024 * 4> buf = {};
    std::ifstream blob_file{blob_path};

    std::streamsize read_byte_numb;
    while (blob_file.good()) {
      blob_file.read(buf.data(), sizeof(buf));
      write_cb(buf.data(), blob_file.gcount(), 1, userp);
    }
    if (!blob_file.eof()) {
      HttpResponse("Failed to read app blob data: " + blob_path, 500, CURLE_OK, "Internal Error");
    }
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse getAppItem(const std::string& url) const {
    const std::string hash_prefix{"sha256:"};
    const auto digest_pos{url.rfind(hash_prefix)};
    if (digest_pos == std::string::npos) {
      return HttpResponse("Invalid URL", 400, CURLE_OK, "");
    }
    const auto hash_pos{digest_pos + hash_prefix.size()};
    const auto hash{url.substr(hash_pos)};
    const auto blob_path{blobs_dir_ / hash};
    if (!boost::filesystem::exists(blob_path)) {
      return HttpResponse("The app blob is missing: " + blob_path.string(), 404, CURLE_OK, "Not found");
    }
    return HttpResponse(Utils::readFile(blobs_dir_ / hash), 200, CURLE_OK, "");
  }

  boost::filesystem::path blobsDir() const { return root_dir_ / "blobs"; }
  const boost::filesystem::path& appsDir() const { return apps_dir_; }
  const boost::filesystem::path& dir() const { return root_dir_; }

 private:
  const boost::filesystem::path root_dir_;
  const std::string hostname_;
  const std::string auth_endpoint_{"https://" + hostname_ + "/token-auth"};
  const boost::filesystem::path apps_dir_{root_dir_ / "apps"};
  const boost::filesystem::path blobs_dir_{root_dir_ / "blobs" / "sha256"};
};

class LocalLiteInstall : public LiteInstall {
 public:
  LocalLiteInstall(std::shared_ptr<LiteClient> client, std::unique_ptr<Uptane::Target> t, std::string& reason,
                   const LocalUpdateSource* local_update_source, InstallMode install_mode = InstallMode::All)
      : LiteInstall(std::move(client), std::move(t), reason, install_mode),
        local_update_source_{*local_update_source},
        offline_update_config_{client_->config},
        ostree_sysroot_{nullptr} {
    // make LiteClient to pull from a local ostree repo
    offline_update_config_.pacman.ostree_server = "file://" + local_update_source_.ostree_repo;

    ostree_sysroot_ = std::make_shared<OSTree::Sysroot>(offline_update_config_.pacman);
    storage_ = INvStorage::newStorage(offline_update_config_.storage, false, StorageClient::kTUF);
    if (offline_update_config_.pacman.type == ComposeAppManager::Name &&
        offline_update_config_.pacman.extra.count("reset_apps") == 0) {
      LOG_ERROR << "Cannot perform offline update if non-restorable app engine is set; set `[pacman].reset_apps = "
                   "\"\"` in the device config.";
      throw std::invalid_argument("Invalid app engine type");
    }
  }

  DownloadResult Download() override {
    auto reason = reason_;
    if (reason.empty()) {
      reason = "Update to " + target_->filename();
    }

    client_->logTarget("Downloading: ", *target_);

    auto downloader = createOfflineDownloader();
    client_->notifyDownloadStarted(*target_, reason);
    auto dr{downloader->Download(Target::toTufTarget(*target_))};
    client_->notifyDownloadFinished(*target_, dr, dr.description);

    return {dr.status, dr.description, dr.destination_path};
  }

 private:
  std::unique_ptr<Downloader> createOfflineDownloader() {
    if (offline_update_config_.pacman.type == RootfsTreeManager::Name) {
      // Download just ostree if this is "ostree" only update
      return std::make_unique<RootfsTreeManager>(offline_update_config_.pacman, offline_update_config_.bootloader,
                                                 storage_, nullptr, ostree_sysroot_, *nulled_key_manager_);
    }

    // Handle DG:/token-auth
    std::shared_ptr<HttpInterface> registry_basic_auth_client{std::make_shared<RegistryBasicAuthClient>()};

    std::shared_ptr<OfflineRegistry> offline_registry{
        std::make_shared<OfflineRegistry>(local_update_source_.app_store)};
    // Handle requests to Registry aimed to download App
    Docker::RegistryClient::Ptr registry_client{std::make_shared<Docker::RegistryClient>(
        registry_basic_auth_client, "",
        [offline_registry](const std::vector<std::string>* v, const std::set<std::string>* s) {
          (void)v;
          (void)s;
          return offline_registry;
        })};

    ComposeAppManager::Config pacman_cfg(offline_update_config_.pacman);
    std::string compose_cmd{pacman_cfg.compose_bin.string()};
    if (boost::filesystem::exists(pacman_cfg.compose_bin) && pacman_cfg.compose_bin.filename().compare("docker") == 0) {
      compose_cmd = boost::filesystem::canonical(pacman_cfg.compose_bin).string() + " ";
      // if it is a `docker` binary then turn it into ` the  `docker compose` command
      // and make sure that the `compose` is actually supported by a given `docker` utility.
      compose_cmd += "compose ";
    }

    std::string docker_host{"unix:///var/run/docker.sock"};
    auto env{boost::this_process::environment()};
    if (env.end() != env.find("DOCKER_HOST")) {
      docker_host = env.get("DOCKER_HOST");
    }

    auto docker_client{local_update_source_.docker_client_ptr != nullptr
                           ? *(reinterpret_cast<Docker::DockerClient::Ptr*>(local_update_source_.docker_client_ptr))
                           : std::make_shared<Docker::DockerClient>()};

#ifdef USE_COMPOSEAPP_ENGINE
    AppEngine::Ptr app_engine{std::make_shared<composeapp::AppEngine>(
        pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, registry_client, docker_client,
        docker_host, compose_cmd, pacman_cfg.composectl_bin.string(), pacman_cfg.storage_watermark,
        Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
        [offline_registry](const Docker::Uri& app_uri, const std::string& image_uri) {
          Docker::Uri uri{Docker::Uri::parseUri(image_uri, false)};
          return "--src-shared-blob-dir " + offline_registry->blobsDir().string() +
                 " oci:" + offline_registry->appsDir().string() + "/" + app_uri.app + "/" + app_uri.digest.hash() +
                 "/images/" + uri.registryHostname + "/" + uri.repo + "/" + uri.digest.hash();
        },
        false, /* don't create containers on install because it makes dockerd check if pinned images
      present in its store what we should avoid until images are registered (hacked) in dockerd store */
        local_update_source_.app_store)};
#else
    AppEngine::Ptr app_engine{std::make_shared<Docker::RestorableAppEngine>(
        pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, registry_client, docker_client,
        pacman_cfg.skopeo_bin.string(), docker_host, compose_cmd, Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
        [offline_registry](const Docker::Uri& app_uri, const std::string& image_uri) {
          Docker::Uri uri{Docker::Uri::parseUri(image_uri, false)};
          return "--src-shared-blob-dir " + offline_registry->blobsDir().string() +
                 " oci:" + offline_registry->appsDir().string() + "/" + app_uri.app + "/" + app_uri.digest.hash() +
                 "/images/" + uri.registryHostname + "/" + uri.repo + "/" + uri.digest.hash();
        },
        false, /* don't create containers on install because it makes dockerd check if pinned images
      present in its store what we should avoid until images are registered (hacked) in dockerd store */
        true   /* indicate that this is an offline client */
        )};
#endif

    return std::make_unique<ComposeAppManager>(offline_update_config_.pacman, offline_update_config_.bootloader,
                                               storage_, nullptr, ostree_sysroot_, *nulled_key_manager_, app_engine);
  }

  const LocalUpdateSource local_update_source_;
  Config offline_update_config_;
  OSTree::Sysroot::Ptr ostree_sysroot_;
  std::shared_ptr<INvStorage> storage_;
  // there is no need in the TLS cert/key manager if it is a local download
  std::unique_ptr<KeyManager> nulled_key_manager_{nullptr};
};

bool AkliteClient::IsInstallationInProgress() const { return client_->getPendingTarget().IsValid(); }

TufTarget AkliteClient::GetPendingTarget() const { return Target::toTufTarget(client_->getPendingTarget()); }

std::unique_ptr<InstallContext> AkliteClient::CheckAppsInSync() const {
  std::unique_ptr<InstallContext> installer = nullptr;
  auto target = std::make_unique<Uptane::Target>(client_->getCurrent());
  if (!client_->appsInSync(*target)) {
    boost::uuids::uuid tmp = boost::uuids::random_generator()();
    auto correlation_id = target->custom_version() + "-" + boost::uuids::to_string(tmp);
    target->setCorrelationId(correlation_id);
    std::string reason = "Sync active target apps";
    installer = std::make_unique<LiteInstall>(client_, std::move(target), reason, InstallMode::All);
  }
  client_->setAppsNotChecked();
  return installer;
}

std::unique_ptr<InstallContext> AkliteClient::Installer(const TufTarget& t, std::string reason,
                                                        std::string correlation_id, InstallMode install_mode,
                                                        const LocalUpdateSource* local_update_source) const {
  if (read_only_) {
    throw std::runtime_error("Can't perform this operation from read-only mode");
  }
  std::unique_ptr<Uptane::Target> target;
  // Make sure the metadata is loaded from storage and valid.
  tuf_repo_->CheckMeta();
  for (const auto& tt : tuf_repo_->GetTargets()) {
    bool target_match{false};
    if (local_update_source == nullptr) {
      target_match = tt == t;
    } else {
      // Don't compare app list since it can be shortlisted in the case of the offline/local update during the checkin
      // caused by the offline/preloading shortlist set in the factory CI config.
      target_match = (tt.Name() == t.Name() && tt.Sha256Hash() == t.Sha256Hash() && tt.Version() == t.Version());
    }
    if (target_match) {
      target = std::make_unique<Uptane::Target>(Target::fromTufTarget(t));
      break;
    }
  }
  if (target == nullptr) {
    const auto uptane_target{Target::fromTufTarget(t)};
    if (Target::isInitial(uptane_target) && client_->wasTargetInstalled(uptane_target)) {
      // if it's "initial target" that is not found in the TUF DB, then check if it's not a fake initial target by
      // verifying that this target has been installed on a device before (the initial target that device is booted on
      // and not installed_versions)
      target = std::make_unique<Uptane::Target>(uptane_target);
    } else {
      LOG_ERROR << "The specified Target is not found among trusted TUF targets:\n"
                << "\tName: " << t.Name() << ", ostree hash: " << t.Sha256Hash() << "\n\tApps: " << t.AppsJson();
      return nullptr;
    }
  }
  if (correlation_id.empty()) {
    boost::uuids::uuid tmp = boost::uuids::random_generator()();
    correlation_id = std::to_string(t.Version()) + "-" + boost::uuids::to_string(tmp);
  }
  if (correlation_id.size() > 63) {
    // The backend will reject this
    throw std::runtime_error("Correlation ID's must be less than 64 bytes");
  }
  target->setCorrelationId(correlation_id);
  if (local_update_source == nullptr) {
    return std::make_unique<LiteInstall>(client_, std::move(target), reason, install_mode);
  } else {
    if (t.Custom().isMember(LocalSrcDirKey)) {
      return std::make_unique<LocalLiteInstall>(client_, std::move(target), reason, local_update_source, install_mode);
    } else {
      LOG_ERROR << "Update content of the specified target is not available locally: \n"
                << "\tName: " << t.Name() << ", ostree hash: " << t.Sha256Hash() << "\n\tApps: " << t.AppsJson();
      return nullptr;
    }
  }
}

InstallResult AkliteClient::CompleteInstallation() {
  data::InstallationResult ir;
  auto install_completed{client_->finalizeInstall(&ir)};
  InstallResult complete_install_res{InstallResult::Status::Failed, ""};
  if (install_completed) {
    if (!client_->isBootFwUpdateInProgress()) {
      complete_install_res = {InstallResult::Status::Ok, ""};
    } else {
      complete_install_res = {InstallResult::Status::OkBootFwNeedsCompletion, ""};
    }
  } else if (ir.needCompletion()) {
    complete_install_res = {InstallResult::Status::NeedsCompletion, ir.description};
  } else {
    complete_install_res = {InstallResult::Status::Failed, ir.description};
  }
  return complete_install_res;
}

TufTarget AkliteClient::GetRollbackTarget() const { return Target::toTufTarget(client_->getRollbackTarget()); }

bool AkliteClient::IsRollback(const TufTarget& t) const {
  Json::Value target_json;
  target_json["hashes"]["sha256"] = t.Sha256Hash();
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target(t.Name(), target_json);

  return client_->isRollback(target);
}

InstallResult AkliteClient::SetSecondaries(const std::vector<SecondaryEcu>& ecus) {
  if (read_only_) {
    throw std::runtime_error("Can't perform this operation from read-only mode");
  }
  std::vector<std::string> hwids;
  Json::Value data;
  for (const auto& ecu : ecus) {
    Json::Value entry;
    entry["target"] = ecu.target_name;
    entry["hwid"] = ecu.hwid;
    data[ecu.serial] = entry;
    hwids.emplace_back(ecu.hwid);
  }
  const HttpResponse response = client_->http_client->put(client_->config.tls.server + "/ecus", data);
  if (!response.isOk()) {
    return InstallResult{InstallResult::Status::Failed, response.getStatusStr()};
  }
  secondary_hwids_ = std::move(hwids);
  return InstallResult{InstallResult::Status::Ok, ""};
}

static std::map<std::string, PublicKey> getTargetsKeys(const std::shared_ptr<aklite::tuf::Repo>& tuf_repo,
                                                       int& out_threshold) {
  const auto root_meta{Utils::parseJSON(tuf_repo->GetRoot(-1))};
  std::set<std::string> targets_key_ids;
  for (const auto& key : root_meta["signed"]["roles"]["targets"]["keyids"]) {
    targets_key_ids.insert(key.asString());
  }

  std::map<std::string, PublicKey> targets_pub_keys;
  for (Json::ValueConstIterator it = root_meta["signed"]["keys"].begin(); it != root_meta["signed"]["keys"].end();
       ++it) {
    const std::string keyid{it.key().asString()};
    if (targets_key_ids.count(keyid) == 1) {
      targets_pub_keys.insert({keyid, PublicKey{*it}});
    }
  }

  out_threshold = root_meta["signed"]["roles"]["targets"]["threshold"].asInt();
  return targets_pub_keys;
}

static Json::Value getAndCheckBundleMeta(const boost::filesystem::path& bundle_tuf_path,
                                         const std::map<std::string, PublicKey>& targets_pub_keys,
                                         const int threshold) {
  int found_valid_sig_number{0};
  const auto bundle_meta_path{bundle_tuf_path / "bundle-targets.json"};
  if (!boost::filesystem::exists(bundle_meta_path)) {
    throw std::runtime_error("Failed to find the bundle metadata: " + bundle_meta_path.string());
  }
  const auto bundle_targets_str{Utils::readFile(bundle_meta_path)};
  auto bundle_targets_json{Utils::parseJSON(bundle_targets_str)};
  if (bundle_targets_json.isNull() || bundle_targets_json.empty() || !bundle_targets_json.isMember("signed") ||
      !bundle_targets_json.isMember("signatures")) {
    throw std::runtime_error(
        "The bundle metadata is invalid;"
        " expecting JSON with `signed` and `signatures` fields; got: \n" +
        bundle_targets_str);
  }
  if (!bundle_targets_json["signatures"].isArray()) {
    throw std::runtime_error("The `signatures` field is not an array; got: \n" + bundle_targets_str);
  }
  const std::string signed_body{Utils::jsonToCanonicalStr(bundle_targets_json["signed"])};
  LOG_INFO << "Checking the bundle metadata singatures (required: " << threshold << ")...";
  for (const auto& signature : bundle_targets_json["signatures"]) {
    if (!signature.isObject() || !signature.isMember("keyid") || !signature["keyid"].isString() ||
        !signature.isMember("sig") || !signature["sig"].isString() || !signature.isMember("method") ||
        !signature["method"].isString()) {
      throw std::runtime_error("Invalid signature metadata was found in the bundle metadata: " +
                               signature.toStyledString());
    }
    const auto sig_key_id{signature["keyid"].asString()};
    if (targets_pub_keys.count(sig_key_id) != 1) {
      throw std::runtime_error("The bundle is signed with an unknown key: " + sig_key_id);
    }
    if (!(targets_pub_keys.at(sig_key_id).VerifySignature(signature["sig"].asString(), signed_body))) {
      throw std::runtime_error("An invalid signature was found for the bundle; key ID: " + sig_key_id);
    }
    LOG_INFO << "\t" << sig_key_id << " : OK";
    ++found_valid_sig_number;
  }
  if (found_valid_sig_number < threshold) {
    throw std::runtime_error(
        "An insufficient number of signatures for the bundle were found; "
        "required: " +
        std::to_string(threshold) + ", found: " + std::to_string(found_valid_sig_number));
  }

  TimeStamp timestamp(bundle_targets_json["signed"]["expires"].asString());
  if (timestamp.IsExpiredAt(TimeStamp::Now())) {
    throw Uptane::ExpiredMetadata("offline bundle", "The offline bundle metadata has expired: " + timestamp.ToString());
  }
  return bundle_targets_json;
}

static std::vector<TufTarget> getBundleTargets(const std::shared_ptr<aklite::tuf::Repo>& tuf_repo,
                                               const Json::Value& bundle_targets_meta) {
  std::vector<TufTarget> bundle_targets;
  const auto tuf_targets{tuf_repo->GetTargets()};
  for (const auto& bundle_target : bundle_targets_meta["signed"]["x-fio-offline-bundle"]["targets"]) {
    for (const auto& target : tuf_targets) {
      if (bundle_target == target.Name()) {
        bundle_targets.push_back(target);
      }
    }
  }
  return bundle_targets;
}

static std::pair<CheckInResult::Status, std::string> checkAndGetBundleTargets(
    const std::shared_ptr<aklite::tuf::Repo>& tuf_repo, const boost::filesystem::path& bundle_tuf_path,
    std::vector<TufTarget>& out_bundle_targets) {
  std::vector<TufTarget> bundle_targets;
  try {
    int threshold{-1};
    std::map<std::string, PublicKey> targets_pub_keys;
    targets_pub_keys = getTargetsKeys(tuf_repo, threshold);
    LOG_INFO << "Targets key IDs: ";
    for (const auto& k : targets_pub_keys) {
      LOG_INFO << "\t" << k.first;
    }

    Json::Value bundle_targets_meta;
    bundle_targets_meta = getAndCheckBundleMeta(bundle_tuf_path, targets_pub_keys, threshold);
    LOG_INFO << "Getting bundle targets TUF metadata...";
    bundle_targets = getBundleTargets(tuf_repo, bundle_targets_meta);
    if (bundle_targets.empty()) {
      throw std::runtime_error("None of the bundle targets are present in the TUF targets of the local TUF repository");
    }
    LOG_INFO << "The following bundle targets are allowed to intall: ";
    for (const auto& t : bundle_targets) {
      LOG_INFO << "\t" << t.Name();
    }
  } catch (const std::exception& exc) {
    return {CheckInResult::Status::BundleMetadataError, exc.what()};
  }
  out_bundle_targets = bundle_targets;
  return {CheckInResult::Status::Ok, ""};
}
