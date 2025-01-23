#include "liteclient.h"

#include <fcntl.h>
#include <sys/file.h>

#include <boost/process.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "aklitereportqueue.h"
#include "composeappmanager.h"
#include "crypto/keymanager.h"
#include "crypto/p11engine.h"
#include "helpers.h"
#include "http/httpclient.h"
#include "primary/reportqueue.h"
#include "rootfstreemanager.h"
#include "storage/invstorage.h"
#include "target.h"
#include "uptane/exceptions.h"
#include "uptane/fetcher.h"

static const size_t MaxDetailsSize{2048};

class OfflineMetaFetcher : public Uptane::IMetadataFetcher {
 public:
  explicit OfflineMetaFetcher(boost::filesystem::path tuf_repo_path, Uptane::Version max_root_ver = Uptane::Version())
      : tuf_repo_path_{std::move(tuf_repo_path)}, max_root_ver_{max_root_ver} {}

  void fetchRole(std::string* result, int64_t /* maxsize */, Uptane::RepositoryType /* repo */,
                 const Uptane::Role& role, Uptane::Version version) const override {
    const boost::filesystem::path meta_file_path{tuf_repo_path_ / version.RoleFileName(role)};

    if (!boost::filesystem::exists(meta_file_path) ||
        (role == Uptane::Role::Root() && max_root_ver_ != Uptane::Version() && max_root_ver_ < version)) {
      std::stringstream ver_str;
      ver_str << version;
      throw aklite::tuf::MetadataNotFoundException(role.ToString(), ver_str.str());
    }

    std::ifstream meta_file_stream(meta_file_path.string());
    *result = {std::istreambuf_iterator<char>(meta_file_stream), std::istreambuf_iterator<char>()};
  }

  void fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                       const Uptane::Role& role) const override {
    fetchRole(result, maxsize, repo, role, Uptane::Version());
  }

 private:
  const boost::filesystem::path tuf_repo_path_;
  const Uptane::Version max_root_ver_;
};

LiteClient::LiteClient(Config config_in, const AppEngine::Ptr& app_engine, const std::shared_ptr<P11EngineGuard>& p11,
                       std::shared_ptr<Uptane::IMetadataFetcher> meta_fetcher)
    : config{std::move(config_in)},
      primary_ecu{Uptane::EcuSerial::Unknown(), ""},
      uptane_fetcher_{std::move(meta_fetcher)} {
  storage = INvStorage::newStorage(config.storage, false, StorageClient::kTUF);
  storage->importData(config.import);

  std::map<std::string, std::string>& raw = config.pacman.extra;
  if (raw.count("tags") == 1) {
    std::string val = raw.at("tags");
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(tags, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }

  if (raw.count("callback_program") == 1) {
    callback_program = raw.at("callback_program");
    if (!boost::filesystem::exists(callback_program)) {
      LOG_ERROR << "callback_program(" << callback_program << ") does not exist";
      callback_program = "";
    }
  }

  // figure out the Docker Registry Auth creds endpoint
  const auto& repo_endpoint = config.uptane.repo_server;
  std::string auth_creds_endpoint = Docker::RegistryClient::DefAuthCredsEndpoint;
  if (!repo_endpoint.empty()) {
    auto endpoint_pos = repo_endpoint.rfind('/');
    if (endpoint_pos != std::string::npos) {
      auth_creds_endpoint = repo_endpoint.substr(0, endpoint_pos);
      auth_creds_endpoint.append("/hub-creds/");
    }
  }
  raw["hub_auth_creds_endpoint"] = auth_creds_endpoint;

  EcuSerials ecu_serials;
  if (!storage->loadEcuSerials(&ecu_serials)) {
    // Set a "random" serial so we don't get warning messages.
    std::string serial = config.provision.primary_ecu_serial;
    std::string hwid = config.provision.primary_ecu_hardware_id;
    if (hwid.empty()) {
      hwid = Utils::getHostname();
    }
    if (serial.empty()) {
      boost::uuids::uuid tmp = boost::uuids::random_generator()();
      serial = boost::uuids::to_string(tmp);
    }
    ecu_serials.emplace_back(Uptane::EcuSerial(serial), Uptane::HardwareIdentifier(hwid));
    storage->storeEcuSerials(ecu_serials);
  }
  primary_ecu = ecu_serials[0];

  auto ostree_sysroot = std::make_shared<OSTree::Sysroot>(config.pacman);

  std::vector<std::string> headers;
  // Add all required request headers to the http client and set them to default values.
  // The current Target related headers are updated after the package manager is initialized
  // and the current Target is determined. At this point they just set to "initial/unknown" value.
  // We have to set all headers at this point because the http client API doesn't allow adding new
  // headers after its initialization, headers can be only modified.
  initRequestHeaders(headers);
  http_client = std::make_shared<HttpClientWithShare>(&headers);

  key_manager_ = std_::make_unique<KeyManager>(storage, config.keymanagerConfig(), p11);
  key_manager_->loadKeys();
  key_manager_->copyCertsToCurl(*http_client);

  if (!uptane_fetcher_) {
    uptane_fetcher_ = std::make_shared<Uptane::Fetcher>(config, http_client);
  }
  report_queue = std_::make_unique<AkLiteReportQueue>(config, http_client, storage, report_queue_run_pause_s_,
                                                      report_queue_event_limit_);

  std::shared_ptr<RootfsTreeManager> basepacman;
  // Deduce a package manager type if not set explicitly by a user
  if (config.pacman.type == PACKAGE_MANAGER_NONE) {
    // If the app engine is defined by a caller or there are both the docker client and docker daemon
    // executables on a system then assume that the compose app package manager should be used.
    if (app_engine != nullptr ||
        (boost::filesystem::exists("/usr/bin/dockerd") && boost::filesystem::exists("/usr/bin/docker"))) {
      config.pacman.type = ComposeAppManager::Name;
    } else {
      config.pacman.type = RootfsTreeManager::Name;
    }
  }
  if (config.pacman.type == ComposeAppManager::Name) {
    if (app_engine == nullptr && config.pacman.extra.count("reset_apps") == 0 && !isRegistered(*key_manager_) &&
        config.pacman.extra.count("enforce_composeapp_engine") == 0) {
      LOG_DEBUG << "Device is not registered and the app engine type is not configured,"
                   " enforcing the use of restorable package manager";
      // Enforce the restorable app engine usage
      config.pacman.extra["reset_apps"] = "";
    }
    basepacman = std::make_shared<ComposeAppManager>(config.pacman, config.bootloader, storage, http_client,
                                                     ostree_sysroot, *key_manager_, app_engine);
  } else if (config.pacman.type == RootfsTreeManager::Name) {
    basepacman = std::make_shared<RootfsTreeManager>(config.pacman, config.bootloader, storage, http_client,
                                                     ostree_sysroot, *key_manager_);
  } else {
    throw std::runtime_error("Unsupported package manager type: " + config.pacman.type);
  }
  if (config.pacman.extra.count("x-fio-test-no-init-target") == 0) {
    basepacman->setInitialTargetIfNeeded(config.provision.primary_ecu_hardware_id);
  }
  package_manager_ = basepacman;
  // After running `setInitialTargetIfNeeded` details of the current target are available, so
  // we should update value of the request headers that are related to the current Target description
  updateRequestHeaders();

  downloader_ = std::dynamic_pointer_cast<Downloader>(package_manager_);
  if (!downloader_) {
    throw std::runtime_error("Invalid package manager: cannot cast to Downloader type");
  }
  installer_ = std::dynamic_pointer_cast<Installer>(package_manager_);
  if (!downloader_) {
    throw std::runtime_error("Invalid package manager: cannot cast to Installer type");
  }
  sysroot_ = ostree_sysroot;
  type_ = getClientType(*key_manager_);
}

LiteClient::~LiteClient() {
  // Make sure all events drained before fully destroying the liteclient instance.
  report_queue.reset(nullptr);
}  // NOLINT(modernize-use-equals-default, hicpp-use-equals-default)

data::InstallationResult LiteClient::finalizePendingUpdate(boost::optional<Uptane::Target>& target) {
  data::InstallationResult ret{data::ResultCode::Numeric::kNeedCompletion, ""};
  InstalledVersionUpdateMode mode = InstalledVersionUpdateMode::kPending;
  bool rollback{false};
  bool app_start_failed{false};
  LOG_INFO << "Finalizing pending installation; Target: " << target->filename() << ", hash: " << target->sha256Hash();

  ret = package_manager_->finalizeInstall(*target);
  if (ret.isSuccess()) {
    LOG_INFO << "Finalization has been completed successfully; Target: " << target->filename();
    mode = InstalledVersionUpdateMode::kCurrent;
  } else {
    rollback = data::ResultCode::Numeric::kInstallFailed == ret.result_code.num_code;
    app_start_failed = data::ResultCode::Numeric::kCustomError == ret.result_code.num_code;
    // update dB Target record clearing flags (is_pending, was_installed)
    if (rollback) {
      const auto current{getCurrent()};
      LOG_ERROR << "Finalization has failed; reason: sysroot rollback at boot time, to: " << current.filename()
                << ", hash: " << current.sha256Hash();
      mode = InstalledVersionUpdateMode::kNone;
    }
    if (app_start_failed) {
      LOG_ERROR << "Finalization has failed; reason: apps start failure, currently booted on Target: "
                << target->filename() << ", hash: " << target->sha256Hash();
      mode = InstalledVersionUpdateMode::kNone;
    }
  }
  if (ret.isSuccess() || rollback || app_start_failed) {
    // mark the given Target as "known" Target which indicates that this is a failing/bad Target.
    storage->saveInstalledVersion("", *target, mode);
  }
  return ret;
}

bool LiteClient::finalizeInstall(data::InstallationResult* ir) {
  data::InstallationResult ret{data::ResultCode::Numeric::kOk, ""};
  boost::optional<Uptane::Target> pending;

  // finalize pending installs
  storage->loadInstalledVersions("", nullptr, &pending);
  if (!!pending) {
    callback("install-final-pre", *pending, "");
    ret = finalizePendingUpdate(pending);
  } else {
    LOG_INFO << "No Pending Installs";
  }

  // write current to /var/sota
  const auto current = getCurrent();
  writeCurrentTarget(current);
  updateRequestHeaders();

  // notify the backend about pending Target installation
  if (!!pending && !ret.needCompletion()) {
    notifyInstallFinished(*pending, ret);
  }

  if (ir != nullptr) {
    *ir = ret;
  }
  return ret.result_code.num_code == data::ResultCode::Numeric::kOk;
}

Uptane::Target LiteClient::getRollbackTarget(bool ignore_ostree_hash, bool allow_current) {
  const auto rollback_hash{ignore_ostree_hash ? ""
                                              : sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kRollback)};

  Uptane::Target rollback_target{Uptane::Target::Unknown()};
  {
    std::vector<Uptane::Target> installed_versions;
    storage->loadPrimaryInstallationLog(
        &installed_versions, true /* make sure that Target has been successfully installed */, allow_current);

    std::vector<Uptane::Target>::reverse_iterator it;
    for (it = installed_versions.rbegin(); it != installed_versions.rend(); it++) {
      if (ignore_ostree_hash || it->sha256Hash() == rollback_hash) {
        rollback_target = *it;
        break;
      }
    }
  }
  return rollback_target;
}

void LiteClient::callback(const char* msg, const Uptane::Target& install_target, const std::string& result) {
  if (callback_program.empty()) {
    return;
  }
  auto env = boost::this_process::environment();
  boost::process::environment env_copy = env;
  env_copy["MESSAGE"] = msg;
  env_copy["CURRENT_TARGET"] = (config.storage.path / "current-target").string();
  auto current = getCurrent();
  if (!current.MatchTarget(Uptane::Target::Unknown())) {
    env_copy["CURRENT_TARGET_NAME"] = current.filename();
  }

  if (!install_target.MatchTarget(Uptane::Target::Unknown())) {
    env_copy["INSTALL_TARGET_NAME"] = install_target.filename();
  }
  if (!result.empty()) {
    env_copy["RESULT"] = result;
  }

  int rc = boost::process::system(callback_program, env_copy);
  if (rc != 0) {
    LOG_ERROR << "Error with callback: " << rc;
  }
}

bool LiteClient::checkForUpdatesBegin() {
  notifyTufUpdateStarted();
  const auto rc = updateImageMeta();
  if (!std::get<0>(rc)) {
    notifyTufUpdateFinished(std::get<1>(rc));
  }
  return std::get<0>(rc);
}

void LiteClient::checkForUpdatesEnd(const Uptane::Target& target) { notifyTufUpdateFinished("", target); }
void LiteClient::checkForUpdatesEndWithFailure(const std::string& err) { notifyTufUpdateFinished(err); }

void LiteClient::notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event) const {
  if (!config.tls.server.empty()) {
    event->custom["targetName"] = t.filename();
    event->custom["version"] = t.custom_version();
    if (event->custom.isMember("details")) {
      const auto detail_str{event->custom["details"].asString()};
      if (detail_str.size() > MaxDetailsSize) {
        static const std::string truncated{"[TRUNCATED]"};
        event->custom["details"] = detail_str.substr(0, MaxDetailsSize - truncated.size()) + truncated;
      }
    }
    report_queue->enqueue(std::move(event));
  }
}

std::tuple<bool, boost::filesystem::path> LiteClient::isRootMetaImportNeeded() {
  std::string data;
  if (storage->loadRoot(&data, Uptane::RepositoryType::Image(), Uptane::Version(2))) {
    LOG_DEBUG << "Root role metadata are already present in DB, skipping their import from FS";
    return {false, ""};
  }
  boost::filesystem::path tuf_meta_src{config.storage.uptane_metadata_path.get("/")};
  if (tuf_meta_src == "/metadata") {
    // if libaktualizr's default value then set aklite's default value
    tuf_meta_src = "/usr/lib/sota/tuf";
  }
  if (!boost::filesystem::exists(tuf_meta_src)) {
    LOG_INFO << "Root metadata are not provisioned to FS, will fetch them from Device Gateway or a flash drive (TOFU)";
    return {false, ""};
  }
  return {true, tuf_meta_src};
}

bool LiteClient::importRootMeta(const boost::filesystem::path& src, Uptane::Version max_ver) {
  bool res{true};
  try {
    OfflineMetaFetcher offline_meta_fetcher{src.string(), max_ver};
    image_repo_.updateRoot(*storage, offline_meta_fetcher);
  } catch (const aklite::tuf::MetadataNotFoundException& exc) {
    // That's OK, it means the latest + 1 root version is not found
    LOG_TRACE << "Not found root role metadata; err: " << exc.what();
  } catch (const Uptane::ExpiredMetadata& exc) {
    // That's OK, the pre-provisioned root versions can expire by the time a system image is flashed on a device
    // and a device starts importing them. The expiration is quite possible for the first two versions.
    LOG_TRACE << "Root role metadata have expired; err: " << exc.what();
  } catch (const std::exception& exc) {
    // If it fails then there is no much we can do about it, just do TOFU
    storage->clearMetadata();
    res = false;
    LOG_ERROR << "Failed to import root role metadata: " << exc.what();
  }
  return res;
}

void LiteClient::importRootMetaIfNeededAndPresent() {
  const auto import{isRootMetaImportNeeded()};
  if (!std::get<0>(import)) {
    return;
  }

  LOG_INFO << "Importing root metadata from a local file system...";
  bool prod{type() == Type::Prod};
  Uptane::Version max_ver{Uptane::Version()};

  if (Type::Undefined == type()) {
    max_ver = Uptane::Version(2);
    LOG_WARNING << "Cannot determine a device type (production or CI). "
                << "Importing the first two versions of root role metadata since they are the same for both production "
                   "and CI...";
  }

  boost::filesystem::path import_src{std::get<1>(import) / (prod ? "prod" : "ci")};
  if (prod && !boost::filesystem::exists(import_src / "1.root.json")) {
    LOG_WARNING
        << "This production device is not provisioned with production root role metadata, will download them from "
           "Device Gateway (TOFU)";
    return;
  }

  if (importRootMeta(import_src, max_ver)) {
    LOG_INFO << "Successfully imported " << (prod ? "production" : "CI") << " root role metadata from " << import_src;
  } else {
    LOG_ERROR << "Failed to import " << (prod ? "production" : "CI") << " root role metadata from " << import_src
              << ", will download them from Device Gateway (TOFU)";
  }
}

bool LiteClient::isPendingTarget(const Uptane::Target& target) const {
  return target.sha256Hash() == getPendingTarget().sha256Hash();
}

Uptane::Target LiteClient::getPendingTarget() const {
  boost::optional<Uptane::Target> pending;
  storage->loadInstalledVersions("", nullptr, &pending);

  return !pending ? Uptane::Target::Unknown() : *pending;
}

bool LiteClient::isBootFwUpdateInProgress() const {
  auto* rootfs_pacman = dynamic_cast<RootfsTreeManager*>(package_manager_.get());
  if (rootfs_pacman == nullptr) {
    LOG_ERROR << "Cannot downcast the package manager to the rootfs package manager";
    return false;
  }

  return rootfs_pacman->bootFwUpdateStatus().isUpdateInProgress();
}

bool LiteClient::wasTargetInstalled(const Uptane::Target& target) const {
  std::vector<Uptane::Target> log;
  storage->loadInstallationLog(config.provision.primary_ecu_serial, &log, true);
  return log.end() != std::find_if(log.begin(), log.end(), [&target](const Uptane::Target& t) {
           return target.filename() == t.filename() && target.sha256Hash() == t.sha256Hash();
         });
}

class DetailedDownloadReport : public EcuDownloadStartedReport {
 public:
  DetailedDownloadReport(const Uptane::EcuSerial& ecu, const std::string& correlation_id, const std::string& details)
      : EcuDownloadStartedReport(ecu, correlation_id) {
    custom["details"] = details;
  }
};

class DetailedDownloadCompletedReport : public EcuDownloadCompletedReport {
 public:
  DetailedDownloadCompletedReport(const Uptane::EcuSerial& ecu, const std::string& correlation_id, bool success,
                                  const std::string& details)
      : EcuDownloadCompletedReport(ecu, correlation_id, success) {
    custom["details"] = details;
  }
};

void LiteClient::notifyTufUpdateStarted() { callback("check-for-update-pre", Uptane::Target::Unknown(), ""); }

void LiteClient::notifyTufUpdateFinished(const std::string& err, const Uptane::Target& t) {
  auto msg = err.empty() ? "OK" : "FAILED: " + err;
  callback("check-for-update-post", t, msg);
}

void LiteClient::notifyDownloadStarted(const Uptane::Target& t, const std::string& reason) {
  callback("download-pre", t, "");
  notify(t, std_::make_unique<DetailedDownloadReport>(primary_ecu.first, t.correlation_id(), reason));
}

void LiteClient::notifyDownloadFinished(const Uptane::Target& t, bool success, const std::string& err_msg) {
  callback("download-post", t, success ? "OK" : "FAILED");
  notify(t,
         std_::make_unique<DetailedDownloadCompletedReport>(primary_ecu.first, t.correlation_id(), success, err_msg));
}

void LiteClient::notifyInstallStarted(const Uptane::Target& t) {
  callback("install-pre", t, "");
  notify(t, std_::make_unique<EcuInstallationStartedReport>(primary_ecu.first, t.correlation_id()));
}

class DetailedAppliedReport : public EcuInstallationAppliedReport {
 public:
  DetailedAppliedReport(const Uptane::EcuSerial& ecu, const std::string& correlation_id, const std::string& details)
      : EcuInstallationAppliedReport(ecu, correlation_id) {
    custom["details"] = details;
  }
};

class DetailedInstallCompletedReport : public EcuInstallationCompletedReport {
 public:
  DetailedInstallCompletedReport(const Uptane::EcuSerial& ecu, const std::string& correlation_id, bool success,
                                 const std::string& details)
      : EcuInstallationCompletedReport(ecu, correlation_id, success) {
    if (!success && details.size() > MaxDetailsSize) {
      static const std::size_t err_tail_size{1024};
      static const std::string truncated{"\n[TRUNCATED]\n"};

      std::size_t head_size{MaxDetailsSize - err_tail_size - truncated.size()};

      custom["details"] =
          details.substr(0, head_size) + truncated + details.substr(details.size() - err_tail_size, err_tail_size);
    } else {
      custom["details"] = details;
    }
  }
};

void LiteClient::notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir) {
  if (ir.needCompletion()) {
    callback("install-post", t, "NEEDS_COMPLETION");
    notify(t, std_::make_unique<DetailedAppliedReport>(primary_ecu.first, t.correlation_id(), ir.description));
    return;
  }

  if (ir.result_code == data::ResultCode::Numeric::kOk) {
    writeCurrentTarget(t);
    callback("install-post", t, "OK");
    notify(t, std_::make_unique<DetailedInstallCompletedReport>(primary_ecu.first, t.correlation_id(), true,
                                                                ir.description));
  } else {
    callback("install-post", t, "FAILED");
    notify(t, std_::make_unique<DetailedInstallCompletedReport>(primary_ecu.first, t.correlation_id(), false,
                                                                ir.description));
  }
}

void LiteClient::writeCurrentTarget(const Uptane::Target& t) const {
  std::stringstream ss;
  ss << "TARGET_NAME=\"" << t.filename() << "\"\n";
  ss << "CUSTOM_VERSION=\"" << t.custom_version() << "\"\n";
  Json::Value custom = t.custom_data();
  std::string tmp = custom["lmp-manifest-sha"].asString();
  if (!tmp.empty()) {
    ss << "LMP_MANIFEST_SHA=\"" << tmp << "\"\n";
  }
  tmp = custom["meta-subscriber-overrides-sha"].asString();
  if (!tmp.empty()) {
    ss << "META_SUBSCRIBER_OVERRIDES_SHA=\"" << tmp << "\"\n";
  }
  tmp = custom["containers-sha"].asString();
  if (!tmp.empty()) {
    ss << "CONTAINERS_SHA=\"" << tmp << "\"\n";
  }

  ss << "TAG=\"";
  const auto& tags = custom["tags"];
  bool first = true;
  for (Json::ValueConstIterator i = tags.begin(); i != tags.end(); ++i) {
    if (!first) {
      ss << ",";
    }
    ss << (*i).asString();
    first = false;
  }
  ss << "\"\n";

  Utils::writeFile(config.storage.path / "current-target", ss.str());
}

data::InstallationResult LiteClient::installPackage(const Uptane::Target& target, InstallMode install_mode) {
  LOG_INFO << "Installing package using " << package_manager_->name() << " package manager";
  try {
    return installer_->Install(Target::toTufTarget(target), install_mode);
  } catch (std::exception& ex) {
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, ex.what());
  }
}

std::tuple<bool, std::string> LiteClient::updateImageMeta() {
  try {
    image_repo_.updateMeta(*storage, *uptane_fetcher_);
  } catch (const Uptane::SecurityException& e) {
    const std::string err{e.what()};
    LOG_ERROR << "Failed to update the local TUF repo; TUF metadata check failure: " << err;
    return {false, err};
  } catch (const Uptane::ExpiredMetadata& e) {
    const std::string err{e.what()};
    LOG_ERROR << "Failed to update the local TUF repo; TUF metadata is expired: " << err;
    return {false, err};
  } catch (const Uptane::MetadataFetchFailure& e) {
    const std::string err{e.what()};
    LOG_ERROR << "Failed to update the local TUF repo; TUF metadata fetch failure: " << err;
    if (err.find("Failed to fetch role timestamp") != std::string::npos) {
      LOG_ERROR << "Check the device tag or verify the existence of a wave for the device";
    }
    return {false, err};
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to update Image repo metadata: " << e.what();
    return {false, e.what()};
  }

  return {true, ""};
}

const std::vector<Uptane::Target>& LiteClient::allTargets() const {
  std::shared_ptr<const Uptane::Targets> targets{image_repo_.getTargets()};
  if (targets) {
    return image_repo_.getTargets()->targets;
  } else {
    return no_targets_;
  }
}

bool LiteClient::checkImageMetaOffline() {
  try {
    image_repo_.checkMetaOffline(*storage);
    return true;
  } catch (const Uptane::SecurityException& e) {
    const std::string err{e.what()};
    LOG_ERROR << "Failed to update the local TUF repo; TUF metadata check failure: " << err;
  } catch (const Uptane::ExpiredMetadata& e) {
    const std::string err{e.what()};
    LOG_ERROR << "Failed to update the local TUF repo; TUF metadata is expired: " << err;
  } catch (const aklite::tuf::MetadataNotFoundException& e) {
    const std::string err{e.what()};
    LOG_ERROR << "Failed to update the local TUF repo; TUF metadata not found: " << err;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to check Image repo metadata: " << e.what();
  }
  return false;
}

DownloadResult LiteClient::downloadImage(const Uptane::Target& target, const api::FlowControlToken* token) {
  key_manager_->loadKeys();

  DownloadResult download_result{DownloadResult::Status::DownloadFailed, ""};
  {
    const int max_tries = 3;
    int tries = 0;
    std::chrono::milliseconds wait(500);

    for (; tries < max_tries; tries++) {
      download_result = downloader_->Download(Target::toTufTarget(target));
      // success = package_manager_->fetchTarget(target, *uptane_fetcher_, *key_manager_, prog_cb, token);

      // Skip trying to fetch the 'target' if control flow token transaction
      // was set to the 'abort' or 'pause' state, see the CommandQueue and FlowControlToken.
      if (download_result || download_result.noSpace() || (token != nullptr && !token->canContinue(false))) {
        break;
      } else if (tries < max_tries - 1) {
        std::this_thread::sleep_for(wait);
        wait *= 2;
      }
    }
    if (!download_result) {
      LOG_ERROR << "Download unsuccessful after " << tries + 1 << " attempts; err: " << download_result.description;
    }
  }

  return download_result;
}

void LiteClient::reportAktualizrConfiguration() {
  if (!config.telemetry.report_config) {
    LOG_DEBUG << "Not reporting libaktualizr configuration because telemetry is disabled";
    return;
  }

  std::stringstream conf_ss;
  config.writeToStream(conf_ss);
  const std::string conf_str = conf_ss.str();
  const Hash new_hash = Hash::generate(Hash::Type::kSha256, conf_str);
  std::string stored_hash;
  if (!(storage->loadDeviceDataHash("configuration", &stored_hash) &&
        new_hash == Hash(Hash::Type::kSha256, stored_hash))) {
    LOG_DEBUG << "Reporting libaktualizr configuration";
    const HttpResponse response =
        http_client->put(config.tls.server + "/system_info/config", "application/toml", conf_str);
    if (response.isOk()) {
      storage->storeDeviceDataHash("configuration", new_hash.HashString());
    } else {
      LOG_DEBUG << "Unable to report libaktualizr configuration: " << response.getStatusStr();
    }
  }
}

void LiteClient::reportNetworkInfo() {
  if (config.telemetry.report_network) {
    LOG_DEBUG << "Reporting network information";
    Json::Value network_info = Utils::getNetworkInfo();
    if (network_info != last_network_info_reported_) {
      const HttpResponse response = http_client->put(config.tls.server + "/system_info/network", network_info);
      if (response.isOk()) {
        last_network_info_reported_ = network_info;
      } else {
        LOG_DEBUG << "Unable to report network information: " << response.getStatusStr();
      }
    }
  } else {
    LOG_DEBUG << "Not reporting network information because telemetry is disabled";
  }
}

void LiteClient::reportHwInfo() {
  if (!config.telemetry.report_network) {
    LOG_DEBUG << "Not reporting hwinfo information because telemetry is disabled";
    return;
  }

  if (hwinfo_reported_) {
    return;
  }
  Json::Value hw_info = Utils::getHardwareInfo();
  if (!hw_info.empty()) {
    const HttpResponse response = http_client->put(config.tls.server + "/system_info", hw_info);
    if (response.isOk()) {
      hwinfo_reported_ = true;
    } else {
      LOG_DEBUG << "Unable to report hwinfo information: " << response.getStatusStr();
    }
  } else {
    LOG_WARNING << "Unable to fetch hardware information from host system.";
  }
}

void LiteClient::reportAppsState() {
  if (package_manager_->name() != ComposeAppManager::Name) {
    return;
  }
  auto compose_pacman = std::dynamic_pointer_cast<ComposeAppManager>(package_manager_);
  if (!compose_pacman) {
    LOG_ERROR << "Cannot downcast the package manager to Compose App Manager";
    return;
  }
  const auto apps_state{compose_pacman->getAppsState()};
  if (apps_state.isNull()) {
    LOG_WARNING << "Failed to obtain Apps state, skipping sending it to Device Gateway";
    return;
  }
  if (ComposeAppManager::compareAppsStates(apps_state_, apps_state)) {
    LOG_DEBUG << "Apps state has not changed, skipping sending it to Device Gateway";
    return;
  }
  auto resp = http_client->post(config.tls.server + "/apps-states", apps_state);
  if (resp.isOk()) {
    apps_state_ = apps_state;
  } else {
    LOG_WARNING << "Failed to send App states to Device Gateway: " << resp.getStatusStr();
  }
}

DownloadResult LiteClient::download(const Uptane::Target& target, const std::string& reason) {
  notifyDownloadStarted(target, reason);
  auto download_result{downloadImage(target)};
  notifyDownloadFinished(target, download_result, download_result.description);
  return download_result;
}

data::ResultCode::Numeric LiteClient::install(const Uptane::Target& target, InstallMode install_mode) {
  notifyInstallStarted(target);
  auto iresult = installPackage(target, install_mode);
  if (iresult.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
    LOG_INFO << "Update complete. Please reboot the device to activate";
    is_reboot_required_ = true;
    if (target.sha256Hash() == sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending)) {
      // Don't mark Target as pending if its ostree deployment is not really pending.
      // It happens if rootfs/ostreemanager::install() detects the boot firmware update during installation
      // and exists the installation earlier before the target ostree is actually deployed.
      // So, we should not mark such target as "pending" to avoid "finalization" just after reboot.
      // So, after the reboot the boot fw update is confirmed and then the aklite will try to install
      // the given Target again.
      storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
    }
  } else if (iresult.result_code.num_code == data::ResultCode::Numeric::kOk) {
    if (install_mode == InstallMode::OstreeOnly) {
      // This is the case when the new Target updates just Apps and the ostree only mode is set.
      // It means that the Apps update is downloaded but not installed.
      LOG_INFO << "Apps have been downloaded. Run finalize to install and run them";
      storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
      iresult = {data::ResultCode::Numeric::kNeedCompletion, "finalization must be called to install and start Apps"};
    } else {
      LOG_INFO << "Update complete. No reboot needed";
      storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kCurrent);
      writeCurrentTarget(target);
      updateRequestHeaders();
    }
  } else if (iresult.result_code.num_code == data::ResultCode::Numeric::kDownloadFailed) {
    LOG_INFO << "Apps installation failed while the install process was trying to load App images to docker store,"
                " will try the install again at the next update cycle.";
  } else {
    LOG_ERROR << "Unable to install update: " << iresult.description;
    LOG_ERROR << "Marking " << target.filename() << " as a failing Target";
    storage->saveInstalledVersion("", target, InstalledVersionUpdateMode::kNone);
    // let go of the lock since we couldn't update
  }
  notifyInstallFinished(target, iresult);
  return iresult.result_code.num_code;
}

bool LiteClient::isTargetActive(const Uptane::Target& target) const {
  const auto current{getCurrent()};
  return target.filename() == current.filename() && target.sha256Hash() == current.sha256Hash();
}

bool LiteClient::appsInSync(const Uptane::Target& target) const { return appsToUpdate(target).empty(); }

ComposeAppManager::AppsSyncReason LiteClient::appsToUpdate(const Uptane::Target& target,
                                                           bool cleanup_removed_apps) const {
  if (package_manager_->name() == ComposeAppManager::Name) {
    auto* compose_pacman = dynamic_cast<ComposeAppManager*>(package_manager_.get());
    if (compose_pacman == nullptr) {
      LOG_ERROR << "Cannot downcast the package manager to a specific type";
      return {};
    }
    LOG_INFO << "Checking status of Active Target (" << target.filename() << ")";
    auto apps_to_update = compose_pacman->checkForAppsToUpdate(target);
    if (cleanup_removed_apps && apps_to_update.empty()) {
      compose_pacman->handleRemovedApps(getCurrent());
    }

    return apps_to_update;
  }
  return {};
}

void LiteClient::setAppsNotChecked() {
  if (package_manager_->name() == ComposeAppManager::Name) {
    auto* compose_pacman = dynamic_cast<ComposeAppManager*>(package_manager_.get());
    if (compose_pacman == nullptr) {
      LOG_ERROR << "Cannot downcast the package manager to a specific type";
    } else {
      compose_pacman->setAppsNotChecked();
    }
  }
}

std::string LiteClient::getDeviceID() const { return key_manager_->getCN(); }

void LiteClient::initRequestHeaders(std::vector<std::string>& headers) const {
  headers.emplace_back("x-ats-ostreehash: unknown");
  headers.emplace_back("x-ats-target: unknown");
  if (config.pacman.type == ComposeAppManager::Name) {
    headers.emplace_back("x-ats-dockerapps: unknown");
  }
  headers.emplace_back("x-ats-tags: " + boost::algorithm::join(tags, ","));
}

void LiteClient::updateRequestHeaders() {
  const auto current(getCurrent());
  http_client->updateHeader("x-ats-target", current.filename());
  http_client->updateHeader("x-ats-ostreehash", current.sha256Hash());
  if (config.pacman.type == ComposeAppManager::Name) {
    http_client->updateHeader("x-ats-dockerapps",
                              Target::appsStr(current, ComposeAppManager::Config(config.pacman).apps));
  }
}

void LiteClient::logTarget(const std::string& prefix, const Uptane::Target& target) const {
  Target::log(
      prefix, target,
      config.pacman.type == ComposeAppManager::Name ? ComposeAppManager::Config(config.pacman).apps : boost::none);
}

bool LiteClient::isRollback(const Uptane::Target& target) {
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*this, known_but_not_installed_versions);
  return known_local_target(*this, target, known_but_not_installed_versions);
}

bool LiteClient::isRegistered(const KeyManager& key_manager) {
  try {
    return !key_manager.getCert().empty();
  } catch (const std::exception& exc) {
    LOG_DEBUG << "Failed to obtain a device certificate, it implies a device is not registered";
  }
  return false;
}

LiteClient::Type LiteClient::getClientType(const KeyManager& key_manager) {
  static const std::string prod_bc_value{"production"};

  Type res{Type::Undefined};
  try {
    const auto bc{key_manager.getBC()};
    if (bc == prod_bc_value) {
      res = Type::Prod;
      LOG_DEBUG << "Found production business category in a device certificate: " << bc;
    } else {
      res = Type::Dev;
      LOG_DEBUG << "Missing or non-production business category found in a device certificate: " << bc;
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "A device certificate is not found or failed to parse it: " << exc.what();
  }
  return res;
}

boost::optional<std::vector<std::string>> LiteClient::getAppShortlist() const {
  return config.pacman.type == ComposeAppManager::Name ? ComposeAppManager::Config(config.pacman).apps : boost::none;
}
