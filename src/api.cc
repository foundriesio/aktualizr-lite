#include "aktualizr-lite/api.h"

#include <sys/file.h>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "helpers.h"
#include "libaktualizr/config.h"
#include "liteclient.h"

std::vector<boost::filesystem::path> AkliteClient::CONFIG_DIRS = {"/usr/lib/sota/conf.d", "/var/sota/sota.toml",
                                                                  "/etc/sota/conf.d/"};

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
  }
  os << res.description;
  return os;
}

std::ostream& operator<<(std::ostream& os, const InstallResult& res) {
  if (res.status == InstallResult::Status::Ok) {
    os << "Ok/";
  } else if (res.status == InstallResult::Status::NeedsCompletion) {
    os << "NeedsCompletion/";
  } else if (res.status == InstallResult::Status::Failed) {
    os << "Failed/";
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

AkliteClient::AkliteClient(const std::vector<boost::filesystem::path>& config_dirs, bool read_only) {
  read_only_ = read_only;
  Config config(config_dirs);
  if (!read_only_) {
    assert_lock();
    config.telemetry.report_network = !config.tls.server.empty();
    config.telemetry.report_config = !config.tls.server.empty();
  }
  client_ = std::make_unique<LiteClient>(config, nullptr);
  if (!read_only_) {
    client_->finalizeInstall();
  }
}

static bool compareTargets(const TufTarget& a, const TufTarget& b) { return a.Version() < b.Version(); }

CheckInResult AkliteClient::CheckIn() const {
  if (!configUploaded_) {
    client_->reportAktualizrConfiguration();
    configUploaded_ = true;
  }
  client_->reportNetworkInfo();
  client_->reportHwInfo();

  auto status = CheckInResult::Status::Ok;
  Uptane::HardwareIdentifier hwidToFind(client_->config.provision.primary_ecu_hardware_id);

  LOG_INFO << "Refreshing Targets metadata";
  const auto rc = client_->updateImageMeta();
  if (!std::get<0>(rc)) {
    LOG_WARNING << "Unable to update latest metadata, using local copy: " << std::get<1>(rc);
    if (!client_->checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      return CheckInResult(CheckInResult::Status::Failed, "", {});
    }
    status = CheckInResult::Status::OkCached;
  }

  std::vector<TufTarget> targets;
  for (const auto& t : client_->allTargets()) {
    int ver = 0;
    try {
      ver = std::stoi(t.custom_version(), nullptr, 0);
    } catch (const std::invalid_argument& exc) {
      LOG_ERROR << "Invalid version number format: " << t.custom_version();
      ver = -1;
    }
    if (!target_has_tags(t, client_->tags)) {
      continue;
    }
    for (const auto& it : t.hardwareIds()) {
      if (it == hwidToFind) {
        targets.emplace_back(t.filename(), t.sha256Hash(), ver, t.custom_data());
        break;
      }
      for (const auto& hwid : secondary_hwids_) {
        if (it == Uptane::HardwareIdentifier(hwid)) {
          targets.emplace_back(t.filename(), t.sha256Hash(), ver, t.custom_data());
          break;
        }
      }
    }
  }

  std::sort(targets.begin(), targets.end(), compareTargets);
  return CheckInResult(status, client_->config.provision.primary_ecu_hardware_id, targets);
}

boost::property_tree::ptree AkliteClient::GetConfig() const {
  std::stringstream ss;
  ss << client_->config;

  boost::property_tree::ptree pt;
  boost::property_tree::ini_parser::read_ini(ss, pt);
  return pt;
}

TufTarget AkliteClient::GetCurrent() const {
  auto current = client_->getCurrent();
  int ver = -1;
  try {
    ver = std::stoi(current.custom_version(), nullptr, 0);
  } catch (const std::invalid_argument& exc) {
    LOG_ERROR << "Invalid version number format: " << current.custom_version();
  }
  return TufTarget(current.filename(), current.sha256Hash(), ver, current.custom_data());
}

class LiteInstall : public InstallContext {
 public:
  LiteInstall(std::shared_ptr<LiteClient> client, std::unique_ptr<Uptane::Target> t, std::string& reason)
      : client_(std::move(client)), target_(std::move(t)), reason_(reason) {}

  InstallResult Install() override {
    client_->logTarget("Installing: ", *target_);

    auto rc = client_->install(*target_);
    auto status = InstallResult::Status::Failed;
    if (rc == data::ResultCode::Numeric::kNeedCompletion) {
      status = InstallResult::Status::NeedsCompletion;
    } else if (rc == data::ResultCode::Numeric::kOk) {
      client_->http_client->updateHeader("x-ats-target", target_->filename());
      status = InstallResult::Status::Ok;
    }
    return InstallResult{status, ""};
  }

  DownloadResult Download() override {
    auto reason = reason_;
    if (reason.empty()) {
      reason = "Update to " + target_->filename();
    }

    client_->logTarget("Downloading: ", *target_);

    data::ResultCode::Numeric rc = client_->download(*target_, reason);
    if (rc != data::ResultCode::Numeric::kOk) {
      return DownloadResult{DownloadResult::Status::DownloadFailed, "Unable to download target"};
    }

    if (client_->VerifyTarget(*target_) != TargetStatus::kGood) {
      data::InstallationResult ires{data::ResultCode::Numeric::kVerificationFailed, "Downloaded target is invalid"};
      client_->notifyInstallFinished(*target_, ires);
      return DownloadResult{DownloadResult::Status::VerificationFailed, ires.description};
    }

    return DownloadResult{DownloadResult::Status::Ok, ""};
  }

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

 private:
  std::shared_ptr<LiteClient> client_;
  std::unique_ptr<Uptane::Target> target_;
  std::string reason_;
};

std::unique_ptr<InstallContext> AkliteClient::CheckAppsInSync() const {
  std::unique_ptr<InstallContext> installer = nullptr;
  if (!client_->appsInSync()) {
    auto target = std::make_unique<Uptane::Target>(client_->getCurrent());
    boost::uuids::uuid tmp = boost::uuids::random_generator()();
    auto correlation_id = target->custom_version() + "-" + boost::uuids::to_string(tmp);
    target->setCorrelationId(correlation_id);
    std::string reason = "Sync active target apps";
    installer = std::make_unique<LiteInstall>(client_, std::move(target), reason);
  }
  client_->setAppsNotChecked();
  return installer;
}

std::unique_ptr<InstallContext> AkliteClient::Installer(const TufTarget& t, std::string reason) const {
  if (read_only_) {
    throw std::runtime_error("Can't perform this operation from read-only mode");
  }
  std::unique_ptr<Uptane::Target> target;
  for (const auto& tt : client_->allTargets()) {
    if (tt.filename() == t.Name()) {
      target = std::make_unique<Uptane::Target>(tt);
      break;
    }
  }
  if (target == nullptr) {
    return nullptr;
  }
  boost::uuids::uuid tmp = boost::uuids::random_generator()();
  auto correlation_id = std::to_string(t.Version()) + "-" + boost::uuids::to_string(tmp);
  target->setCorrelationId(correlation_id);
  return std::make_unique<LiteInstall>(client_, std::move(target), reason);
}

bool AkliteClient::IsRollback(const TufTarget& t) const {
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*client_, known_but_not_installed_versions);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = t.Sha256Hash();
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target(t.Name(), target_json);

  return known_local_target(*client_, target, known_but_not_installed_versions);
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
