#include "liteclient.h"

#include <fcntl.h>
#include <sys/file.h>

#include <boost/container/flat_map.hpp>
#include <boost/process.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "composeappmanager.h"
#include "crypto/keymanager.h"

#include "target.h"

LiteClient::LiteClient(Config& config_in)
    : config{std::move(config_in)},
      primary_ecu_{Uptane::EcuSerial::Unknown(), config.provision.primary_ecu_hardware_id} {
  std::string pkey;
  storage = INvStorage::newStorage(config.storage);
  storage->importData(config.import);

  const std::map<std::string, std::string> raw = config.pacman.extra;
  if (raw.count("tags") == 1) {
    std::string val = raw.at("tags");
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(tags_, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }

  if (raw.count("callback_program") == 1) {
    callback_program = raw.at("callback_program");
    if (!boost::filesystem::exists(callback_program)) {
      LOG_ERROR << "callback_program(" << callback_program << ") does not exist";
      callback_program = "";
    }
  }

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
  primary_ecu_ = ecu_serials[0];

  std::vector<std::string> headers;
  if (config.pacman.extra.count("booted") == 1) {
    booted_sysroot = boost::lexical_cast<bool>(config.pacman.extra.at("booted"));
  }

  auto ostree_sysroot = std::make_shared<OSTree::Sysroot>(config.pacman.sysroot.string(), booted_sysroot);
  auto cur_hash = ostree_sysroot->getCurDeploymentHash();

  std::string header("x-ats-ostreehash: ");
  if (!cur_hash.empty()) {
    header += cur_hash;
  } else {
    header += "?";
  }
  headers.push_back(header);
  add_apps_header(headers, config.pacman);

  headers.emplace_back("x-ats-target: unknown");

  if (!config.telemetry.report_network) {
    // Provide the random primary ECU serial so our backend will have some
    // idea of the number of unique devices using the system
    headers.emplace_back("x-ats-primary: " + primary_ecu_.first.ToString());
  }

  headers.emplace_back("x-ats-tags: " + boost::algorithm::join(tags_, ","));

  http_client = std::make_shared<HttpClient>(&headers);
  uptane_fetcher_ = std::make_shared<Uptane::Fetcher>(config, http_client);
  report_queue = std_::make_unique<ReportQueue>(config, http_client, storage);

  key_manager_ = std_::make_unique<KeyManager>(storage, config.keymanagerConfig());
  key_manager_->loadKeys();
  key_manager_->copyCertsToCurl(*http_client);

  if (config.pacman.type == ComposeAppManager::Name) {
    package_manager_ =
        std::make_shared<ComposeAppManager>(config.pacman, config.bootloader, storage, http_client, ostree_sysroot);
  } else if (config.pacman.type == PACKAGE_MANAGER_OSTREE) {
    package_manager_ = std::make_shared<OstreeManager>(config.pacman, config.bootloader, storage, http_client);
  } else {
    throw std::runtime_error("Unsupported package manager type: " + config.pacman.type);
  }

  {
    // finalize a pending update if any
    boost::optional<Uptane::Target> pending_target;
    storage->loadInstalledVersions("", nullptr, &pending_target);

    if (!!pending_target) {
      data::InstallationResult update_finalization_result = package_manager_->finalizeInstall(*pending_target);
      if (update_finalization_result.isSuccess()) {
        LOG_INFO << "Marking target install complete for: " << *pending_target;
        storage->saveInstalledVersion("", *pending_target, InstalledVersionUpdateMode::kCurrent);
      }

      if (update_finalization_result.result_code != data::ResultCode::Numeric::kAlreadyProcessed ||
          update_finalization_result.result_code != data::ResultCode::Numeric::kNeedCompletion) {
        notifyInstallFinished(*pending_target, update_finalization_result);
      }
    }
  }

  const auto current_target = getCurrent();
  update_request_headers(http_client, current_target, config.pacman);
  writeCurrentTarget(current_target);
}

Uptane::Target LiteClient::getCurrent(bool refresh) {
  if (refresh || !current_target_.IsValid()) {
    current_target_ = package_manager_->getCurrent();
  }
  return current_target_;
}

bool LiteClient::checkForUpdates() {
  Uptane::Target t = Uptane::Target::Unknown();
  callback("check-for-update-pre", t);
  bool rc = updateImageMeta();
  callback("check-for-update-post", t);
  return rc;
}

std::unique_ptr<Uptane::Target> LiteClient::getTarget(const std::string& version) {
  std::unique_ptr<Uptane::Target> rv;
  if (!updateImageMeta()) {
    LOG_WARNING << "Unable to update latest metadata, using local copy";
    if (!checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      throw std::runtime_error("Unable to find update");
    }
  }

  // if a new version of targets.json hasn't been downloaded why do we do the following search
  // for the latest ??? It's really needed just for the forced update to a specific version
  bool find_latest = (version == "latest");
  std::unique_ptr<Uptane::Target> latest = nullptr;
  for (const auto& t : allTargets()) {
    if (!t.IsValid()) {
      continue;
    }

    if (!t.IsOstree()) {
      continue;
    }

    if (!Target::hasTag(t, tags_)) {
      continue;
    }
    for (auto const& it : t.hardwareIds()) {
      if (it == primary_ecu_.second) {
        if (find_latest) {
          if (latest == nullptr || Target::Version(latest->custom_version()) < Target::Version(t.custom_version())) {
            latest = std_::make_unique<Uptane::Target>(t);
          }
        } else if (version == t.filename() || version == t.custom_version()) {
          return std_::make_unique<Uptane::Target>(t);
        }
      }
    }
  }
  if (find_latest && latest != nullptr) {
    return latest;
  }
  throw std::runtime_error("Unable to find update");
}

boost::container::flat_map<int, Uptane::Target> LiteClient::getTargets() {
  // TODO: bring getTarget and getTargets to common ground
  boost::container::flat_map<int, Uptane::Target> sorted_targets;
  for (const auto& t : allTargets()) {
    int ver = 0;
    try {
      ver = std::stoi(t.custom_version(), nullptr, 0);
    } catch (const std::invalid_argument& exc) {
      LOG_ERROR << "Invalid version number format: " << t.custom_version();
      ver = -1;
    }
    if (!Target::hasTag(t, tags_)) {
      continue;
    }
    for (const auto& it : t.hardwareIds()) {
      if (it == primary_ecu_.second) {
        sorted_targets.emplace(ver, t);
        break;
      }
    }
  }
  return sorted_targets;
}

void LiteClient::callback(const char* msg, const Uptane::Target& install_target, const std::string& result) {
  if (callback_program.empty()) {
    return;
  }
  auto env = boost::this_process::environment();
  boost::process::environment env_copy = env;
  env_copy["MESSAGE"] = msg;
  env_copy["CURRENT_TARGET"] = (config.storage.path / "current-target").string();

  if (!install_target.MatchTarget(Uptane::Target::Unknown())) {
    env_copy["INSTALL_TARGET"] = install_target.filename();
  }
  if (!result.empty()) {
    env_copy["RESULT"] = result;
  }

  int rc = boost::process::system(callback_program, env_copy);
  if (rc != 0) {
    LOG_ERROR << "Error with callback: " << rc;
  }
}

void LiteClient::notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event) {
  if (!config.tls.server.empty()) {
    event->custom["targetName"] = t.filename();
    event->custom["version"] = t.custom_version();
    report_queue->enqueue(std::move(event));
  }
}

class DetailedDownloadReport : public EcuDownloadStartedReport {
 public:
  DetailedDownloadReport(const Uptane::EcuSerial& ecu, const std::string& correlation_id, const std::string& details)
      : EcuDownloadStartedReport(ecu, correlation_id) {
    custom["details"] = details;
  }
};

void LiteClient::notifyDownloadStarted(const Uptane::Target& t, const std::string& reason) {
  callback("download-pre", t);
  notify(t, std_::make_unique<DetailedDownloadReport>(primary_ecu_.first, t.correlation_id(), reason));
}

void LiteClient::notifyDownloadFinished(const Uptane::Target& t, bool success) {
  callback("download-post", t, success ? "OK" : "FAILED");
  notify(t, std_::make_unique<EcuDownloadCompletedReport>(primary_ecu_.first, t.correlation_id(), success));
}

void LiteClient::notifyInstallStarted(const Uptane::Target& t) {
  callback("install-pre", t);
  notify(t, std_::make_unique<EcuInstallationStartedReport>(primary_ecu_.first, t.correlation_id()));
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
    custom["details"] = details;
  }
};

void LiteClient::notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir) {
  if (ir.needCompletion()) {
    callback("install-post", t, "NEEDS_COMPLETION");
    notify(t, std_::make_unique<DetailedAppliedReport>(primary_ecu_.first, t.correlation_id(), ir.description));
    return;
  }

  if (ir.result_code == data::ResultCode::Numeric::kOk) {
    callback("install-post", t, "OK");
    writeCurrentTarget(t);
    notify(t, std_::make_unique<DetailedInstallCompletedReport>(primary_ecu_.first, t.correlation_id(), true,
                                                                ir.description));
  } else {
    callback("install-post", t, "FAILED");
    notify(t, std_::make_unique<DetailedInstallCompletedReport>(primary_ecu_.first, t.correlation_id(), false,
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
  Utils::writeFile(config.storage.path / "current-target", ss.str());
}

data::InstallationResult LiteClient::installPackage(const Uptane::Target& target) {
  LOG_INFO << "Installing package using " << package_manager_->name() << " package manager";
  try {
    return package_manager_->install(target);
  } catch (std::exception& ex) {
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, ex.what());
  }
}

bool LiteClient::updateImageMeta() {
  try {
    image_repo_.updateMeta(*storage, *uptane_fetcher_);
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to update Image repo metadata: " << e.what();
    return false;
  }

  return true;
}

bool LiteClient::checkImageMetaOffline() {
  try {
    image_repo_.checkMetaOffline(*storage);
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to check Image repo metadata: " << e.what();
    return false;
  }
  return true;
}

std::pair<bool, Uptane::Target> LiteClient::downloadImage(const Uptane::Target& target,
                                                          const api::FlowControlToken* token) {
  key_manager_->loadKeys();
  auto prog_cb = [this](const Uptane::Target& t, const std::string& description, unsigned int progress) {
    // report_progress_cb(events_channel.get(), t, description, progress);
    // TODO: consider make use of it for download progress reporting
  };

  bool success = false;
  {
    const int max_tries = 3;
    int tries = 0;
    std::chrono::milliseconds wait(500);

    for (; tries < max_tries; tries++) {
      success = package_manager_->fetchTarget(target, *uptane_fetcher_, *key_manager_, prog_cb, token);
      // Skip trying to fetch the 'target' if control flow token transaction
      // was set to the 'abort' or 'pause' state, see the CommandQueue and FlowControlToken.
      if (success || (token != nullptr && !token->canContinue(false))) {
        break;
      } else if (tries < max_tries - 1) {
        std::this_thread::sleep_for(wait);
        wait *= 2;
      }
    }
    if (!success) {
      LOG_ERROR << "Download unsuccessful after " << tries << " attempts.";
    }
  }

  return {success, target};
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
  Json::Value hw_info = Utils::getHardwareInfo();
  if (!hw_info.empty()) {
    if (hw_info != last_hw_info_reported_) {
      const HttpResponse response = http_client->put(config.tls.server + "/system_info", hw_info);
      if (response.isOk()) {
        last_hw_info_reported_ = hw_info;
      } else {
        LOG_DEBUG << "Unable to report hwinfo information: " << response.getStatusStr();
      }
    }
  } else {
    LOG_WARNING << "Unable to fetch hardware information from host system.";
  }
}

static std::unique_ptr<Lock> create_lock(boost::filesystem::path lockfile) {
  if (lockfile.empty()) {
    // Just return a dummy one that will safely "close"
    return std_::make_unique<Lock>(-1);
  }

  int fd = open(lockfile.c_str(), O_RDWR | O_CREAT | O_APPEND, 0666);
  if (fd < 0) {
    LOG_ERROR << "Unable to open lock file " << lockfile;
    return nullptr;
  }
  LOG_INFO << "Acquiring lock";
  if (flock(fd, LOCK_EX) < 0) {
    LOG_ERROR << "Unable to acquire lock on " << lockfile;
    close(fd);
    return nullptr;
  }
  return std_::make_unique<Lock>(fd);
}

std::unique_ptr<Lock> LiteClient::getDownloadLock() const { return create_lock(download_lockfile); }
std::unique_ptr<Lock> LiteClient::getUpdateLock() const { return create_lock(update_lockfile); }

data::ResultCode::Numeric LiteClient::download(const Uptane::Target& target, const std::string& reason) {
  std::unique_ptr<Lock> lock = getDownloadLock();
  if (lock == nullptr) {
    return data::ResultCode::Numeric::kInternalError;
  }
  notifyDownloadStarted(target, reason);
  if (!downloadImage(target).first) {
    notifyDownloadFinished(target, false);
    return data::ResultCode::Numeric::kDownloadFailed;
  }
  notifyDownloadFinished(target, true);
  return data::ResultCode::Numeric::kOk;
}

data::ResultCode::Numeric LiteClient::install(const Uptane::Target& target) {
  std::unique_ptr<Lock> lock = getUpdateLock();
  if (lock == nullptr) {
    return data::ResultCode::Numeric::kInternalError;
  }

  notifyInstallStarted(target);
  auto iresult = installPackage(target);
  if (iresult.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
    LOG_INFO << "Update complete. Please reboot the device to activate";
    storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
    is_reboot_required_ = booted_sysroot;
  } else if (iresult.result_code.num_code == data::ResultCode::Numeric::kOk) {
    LOG_INFO << "Update complete. No reboot needed";
    storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kCurrent);
  } else {
    LOG_ERROR << "Unable to install update: " << iresult.description;
    // let go of the lock since we couldn't update
  }
  notifyInstallFinished(target, iresult);
  return iresult.result_code.num_code;
}

bool LiteClient::isTargetActive(const Uptane::Target& target) const {
  return target.filename() == getCurrent().filename();
}

bool LiteClient::appsInSync() const {
  if (package_manager_->name() == ComposeAppManager::Name) {
    auto* compose_pacman = dynamic_cast<ComposeAppManager*>(package_manager_.get());
    if (compose_pacman == nullptr) {
      LOG_ERROR << "Cannot downcast the package manager to a specific type";
      return false;
    }
    LOG_INFO << "Checking Active Target status...";
    auto no_any_app_to_update = compose_pacman->checkForAppsToUpdate(getCurrent());
    if (no_any_app_to_update) {
      compose_pacman->handleRemovedApps(getCurrent());
    }

    return no_any_app_to_update;
  }
  return true;
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

void LiteClient::add_apps_header(std::vector<std::string>& headers, PackageConfig& config) {
  if (config.type == ComposeAppManager::Name) {
    ComposeAppManager::Config cfg(config);
    // TODO: consider this header renaming
    if (!!cfg.apps) {
      headers.emplace_back("x-ats-dockerapps: " + boost::algorithm::join(*cfg.apps, ","));
    } else {
      headers.emplace_back("x-ats-dockerapps: ");
    }
  }
}

void LiteClient::update_request_headers(std::shared_ptr<HttpClient>& http_client, const Uptane::Target& target,
                                        PackageConfig& config) {
  http_client->updateHeader("x-ats-target", target.filename());

  if (config.type == ComposeAppManager::Name) {
    ComposeAppManager::Config cfg(config);

    // If App list was not specified in the config then we need to update the request header with a list of
    // Apps specified in the currently installed Target
    if (!cfg.apps) {
      std::list<std::string> apps;
      auto target_apps = target.custom_data()["docker_compose_apps"];
      for (Json::ValueIterator ii = target_apps.begin(); ii != target_apps.end(); ++ii) {
        if ((*ii).isObject() && (*ii).isMember("uri")) {
          const auto& target_app_name = ii.key().asString();
          apps.push_back(target_app_name);
        }
      }
      http_client->updateHeader("x-ats-dockerapps", boost::algorithm::join(apps, ","));
    }
  }
}
