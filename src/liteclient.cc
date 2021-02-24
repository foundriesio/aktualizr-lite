#include "liteclient.h"

#include <fcntl.h>
#include <sys/file.h>
#include <vector>

#include <boost/container/flat_map.hpp>
#include <boost/process.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "composeappmanager.h"
#include "crypto/keymanager.h"

#include "target.h"

LiteClient::LiteClient(Config& config_in, const boost::program_options::variables_map* const variables_map)
    : config_{std::move(config_in)},
      primary_ecu_{Uptane::EcuSerial::Unknown(), config_.provision.primary_ecu_hardware_id} {
  storage_ = INvStorage::newStorage(config_.storage);
  storage_->importData(config_.import);

  if (variables_map != nullptr) {
    if ((*variables_map).count("update-lockfile") > 0) {
      update_lockfile_ = (*variables_map)["update-lockfile"].as<boost::filesystem::path>();
    }
    if ((*variables_map).count("download-lockfile") > 0) {
      download_lockfile_ = (*variables_map)["download-lockfile"].as<boost::filesystem::path>();
    }

    update_interval_ = config_.uptane.polling_sec;
    if ((*variables_map).count("interval") > 0) {
      update_interval_ = (*variables_map)["interval"].as<uint64_t>();
    }

    if ((*variables_map).count("clear-installed-versions") > 0) {
      LOG_WARNING << "Clearing installed version history!!!";
      storage_->clearInstalledVersions();
    }
  }

  if (config_.uptane.repo_server.empty()) {
    throw std::invalid_argument("[uptane]/repo_server is not configured");
  }

  if (access(config_.bootloader.reboot_command.c_str(), X_OK) != 0) {
    throw std::invalid_argument("reboot command: " + config_.bootloader.reboot_command + " is not executable");
  }

  const std::map<std::string, std::string> raw = config_.pacman.extra;
  if (raw.count("tags") == 1) {
    std::string val = raw.at("tags");
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(tags_, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }

  if (raw.count("callback_program") == 1) {
    callback_program_ = raw.at("callback_program");
    if (!boost::filesystem::exists(callback_program_)) {
      LOG_ERROR << "callback_program(" << callback_program_ << ") does not exist";
      callback_program_ = "";
    }
  }

  EcuSerials ecu_serials;
  if (!storage_->loadEcuSerials(&ecu_serials)) {
    // Set a "random" serial so we don't get warning messages.
    std::string serial = config_.provision.primary_ecu_serial;
    std::string hwid = config_.provision.primary_ecu_hardware_id;
    if (hwid.empty()) {
      hwid = Utils::getHostname();
    }
    if (serial.empty()) {
      boost::uuids::uuid tmp = boost::uuids::random_generator()();
      serial = boost::uuids::to_string(tmp);
    }
    ecu_serials.emplace_back(Uptane::EcuSerial(serial), Uptane::HardwareIdentifier(hwid));
    storage_->storeEcuSerials(ecu_serials);
  }
  primary_ecu_ = ecu_serials[0];

  std::vector<std::string> headers;
  if (config_.pacman.extra.count("booted") == 1) {
    booted_sysroot = boost::lexical_cast<bool>(config_.pacman.extra.at("booted"));
  }

  auto ostree_sysroot = std::make_shared<OSTree::Sysroot>(config_.pacman.sysroot.string(), booted_sysroot);
  auto cur_hash = ostree_sysroot->getCurDeploymentHash();

  std::string header("x-ats-ostreehash: ");
  if (!cur_hash.empty()) {
    header += cur_hash;
  } else {
    header += "?";
  }
  headers.push_back(header);
  addAppsHeader(headers, config_.pacman);

  headers.emplace_back("x-ats-target: unknown");
  if (!config_.telemetry.report_network) {
    // Provide the random primary ECU serial so our backend will have some
    // idea of the number of unique devices using the system
    headers.emplace_back("x-ats-primary: " + primary_ecu_.first.ToString());
  }

  headers.emplace_back("x-ats-tags: " + boost::algorithm::join(tags_, ","));

  http_client_ = std::make_shared<HttpClient>(&headers);
  uptane_fetcher_ = std::make_shared<Uptane::Fetcher>(config_, http_client_);
  report_queue_ = std_::make_unique<ReportQueue>(config_, http_client_, storage_);

  key_manager_ = std_::make_unique<KeyManager>(storage_, config_.keymanagerConfig());
  key_manager_->loadKeys();
  key_manager_->copyCertsToCurl(*http_client_);

  if (config_.pacman.type == ComposeAppManager::Name) {
    auto compose_package_manager =
        std::make_shared<ComposeAppManager>(config_.pacman, config_.bootloader, storage_, http_client_, ostree_sysroot);

    app_shortlist_ = compose_package_manager->getAppShortlist();
    package_manager_ = compose_package_manager;

  } else if (config_.pacman.type == PACKAGE_MANAGER_OSTREE) {
    package_manager_ = std::make_shared<OstreeManager>(config_.pacman, config_.bootloader, storage_, http_client_);
  } else {
    throw std::runtime_error("Unsupported package manager type: " + config_.pacman.type);
  }

  data::InstallationResult update_finalization_result{data::ResultCode::Numeric::kNeedCompletion, ""};

  // check if there is a pending update/installation/Target
  boost::optional<Uptane::Target> pending_target;
  storage_->loadInstalledVersions("", nullptr, &pending_target);
  // finalize a pending update/installation if any
  if (!!pending_target) {
    // if there is a pending update/installation/Target then try to apply/finalize it
    update_finalization_result = package_manager_->finalizeInstall(*pending_target);
    if (update_finalization_result.isSuccess()) {
      LOG_INFO << "Marking target install complete for: " << *pending_target;
      storage_->saveInstalledVersion("", *pending_target, InstalledVersionUpdateMode::kCurrent);
    } else if (data::ResultCode::Numeric::kInstallFailed == update_finalization_result.result_code.num_code) {
      // rollback has happenned, unset is_pending and was_installed flags of the given Target record in DB
      storage_->saveInstalledVersion("", *pending_target, InstalledVersionUpdateMode::kNone);
    }
  }

  updateRequestHeaders();
  writeCurrentTarget();

  if (data::ResultCode::Numeric::kNeedCompletion != update_finalization_result.result_code.num_code) {
    // if finalization has happened, either succefully (kOk) or not (kInstallFailed)
    // we send the installation finished report event.
    notifyInstallFinished(*pending_target, update_finalization_result);
  }
  setInvalidTargets();
}

void LiteClient::reportStatus() {
  reportAktualizrConfiguration();
  reportNetworkInfo();
  reportHwInfo();
}

bool LiteClient::refreshMetadata() {
  bool rc = updateImageMeta();
  if (!rc) {
    LOG_WARNING << "Unable to update latest metadata, using local copy";
    rc = checkImageMetaOffline();
  }
  return rc;
}

Uptane::Target LiteClient::getCurrent(bool refresh) const {
  if (refresh || !current_target_.IsValid()) {
    current_target_ = package_manager_->getCurrent();
  }
  return current_target_;
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

// Update a device to the given Target version
data::ResultCode::Numeric LiteClient::update(const std::string& version, bool force_update) {
  checkForUpdates();

  std::unique_ptr<Uptane::Target> desired_target{getTarget(version)};
  auto current_target = getCurrent(true);

  if (!isTargetValid(*desired_target)) {
    LOG_WARNING << "Target " << (*desired_target).filename() << ", hash " << (*desired_target).sha256Hash()
                << " known locally (rollback?), skipping";
    // if the desired Target is invalid then just sync the current Target
    // maybe return an error/throw exception and don't sync currently installed Target???
    // but in this case the current Target is never synced until a new valid Target becomes available
    // return UpdateType::kCurrentTargetSync;
    desired_target = getTarget(current_target.filename());
  }

  UpdateType update_type = determineUpdateType(*desired_target, current_target, force_update);

  std::string update_reason;
  Uptane::Target update_target{Uptane::Target::Unknown()};
  std::tie(update_reason, update_target) = determineUpdateTarget(update_type, *desired_target, current_target);

  logUpdate(update_type, *desired_target, current_target);

  data::ResultCode::Numeric update_result = doUpdate(*desired_target, update_target, update_reason);

  if (update_result == data::ResultCode::Numeric::kOk) {
    const auto& new_current = getCurrent(true);
    http_client_->updateHeader("x-ats-target", new_current.filename());
  }

  return update_result;
}

std::pair<bool, std::string> LiteClient::isRebootRequired() const {
  return {is_reboot_required_, config_.bootloader.reboot_command};
}

std::string LiteClient::getDeviceID() const { return key_manager_->getCN(); }

std::tuple<bool, Json::Value> LiteClient::getDeviceInfo() {
  const auto http_res = http_client_->get(config_.tls.server + "/device", HttpInterface::kNoLimit);

  bool res{false};
  Json::Value device_info;

  if (http_res.isOk()) {
    device_info = http_res.getJson();
    res = true;
  } else {
    device_info["err"] = http_res.getStatusStr();
  }

  return {res, device_info};
}

void LiteClient::logTarget(const std::string& prefix, const Uptane::Target& target) const {
  Target::log(prefix, target, app_shortlist_);
}

/****** protected and private methods *****/
void LiteClient::reportAktualizrConfiguration() {
  if (!config_.telemetry.report_config) {
    LOG_DEBUG << "Not reporting libaktualizr configuration because telemetry is disabled";
    return;
  }

  std::stringstream conf_ss;
  config_.writeToStream(conf_ss);
  const std::string conf_str = conf_ss.str();
  const Hash new_hash = Hash::generate(Hash::Type::kSha256, conf_str);
  std::string stored_hash;
  if (!(storage_->loadDeviceDataHash("configuration", &stored_hash) &&
        new_hash == Hash(Hash::Type::kSha256, stored_hash))) {
    LOG_DEBUG << "Reporting libaktualizr configuration";
    const HttpResponse response =
        http_client_->put(config_.tls.server + "/system_info/config", "application/toml", conf_str);
    if (response.isOk()) {
      storage_->storeDeviceDataHash("configuration", new_hash.HashString());
    } else {
      LOG_DEBUG << "Unable to report libaktualizr configuration: " << response.getStatusStr();
    }
  }
}

void LiteClient::reportNetworkInfo() {
  if (config_.telemetry.report_network) {
    LOG_DEBUG << "Reporting network information";
    Json::Value network_info = Utils::getNetworkInfo();
    if (network_info != last_network_info_reported_) {
      const HttpResponse response = http_client_->put(config_.tls.server + "/system_info/network", network_info);
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
  if (!config_.telemetry.report_network) {
    LOG_DEBUG << "Not reporting hwinfo information because telemetry is disabled";
    return;
  }
  Json::Value hw_info = Utils::getHardwareInfo();
  if (!hw_info.empty()) {
    if (hw_info != last_hw_info_reported_) {
      const HttpResponse response = http_client_->put(config_.tls.server + "/system_info", hw_info);
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

void LiteClient::checkForUpdates() {
  LOG_INFO << "Checking for a new Target...";

  Uptane::Target t = Uptane::Target::Unknown();
  // why do we send Uptane::Target::Unknown() to the backend???
  callback("check-for-update-pre", t);
  bool rc = refreshMetadata();
  callback("check-for-update-post", t);

  if (!rc) {
    LOG_ERROR << "Unable to use local copy of TUF data";
    throw std::runtime_error("Unable to find update");
  }
}

bool LiteClient::updateImageMeta() {
  try {
    image_repo_.updateMeta(*storage_, *uptane_fetcher_);
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to update Image repo metadata: " << e.what();
    return false;
  }

  return true;
}

bool LiteClient::checkImageMetaOffline() {
  try {
    image_repo_.checkMetaOffline(*storage_);
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to check Image repo metadata: " << e.what();
    return false;
  }
  return true;
}

std::unique_ptr<Uptane::Target> LiteClient::getTarget(const std::string& version) {
  std::unique_ptr<Uptane::Target> rv;
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

bool LiteClient::isTargetValid(const Uptane::Target& target) {
  const auto current = getCurrent();

  // Make sure that Target is not what is currently installed
  if (target.sha256Hash() == current.sha256Hash()) {
    return true;
  }

  boost::optional<Uptane::Target> pending;
  storage_->loadPrimaryInstalledVersions(nullptr, &pending);

  // Make sure that Target is not what is currently pending
  if (!!pending && (target.sha256Hash() == pending->sha256Hash())) {
    return true;
  }

  std::vector<Uptane::Target>::const_reverse_iterator it;
  for (it = invalid_targets_.rbegin(); it != invalid_targets_.rend(); it++) {
    if (it->sha256Hash() == target.sha256Hash()) {
      return false;
    }
  }

  return true;
}

LiteClient::UpdateType LiteClient::determineUpdateType(const Uptane::Target& desired_target,
                                                       const Uptane::Target& current_target, bool force_update) {
  UpdateType update_type = UpdateType::kCurrentTargetSync;
  if (force_update) {
    // If the desired Target is valid and it's forced update then just do the forced update
    // of the specified desired Target regardless of the currently running Target
    update_type = UpdateType::kTargetForcedUpdate;
  } else if (desired_target.filename() != current_target.filename()) {
    // if it's a new Target and not forced update
    update_type = UpdateType::kNewTargetUpdate;
  }
  return update_type;
}

std::tuple<std::string, Uptane::Target> LiteClient::determineUpdateTarget(UpdateType update_type,
                                                                          const Uptane::Target& desired_target,
                                                                          const Uptane::Target& current_target) {
  Uptane::Target update_target{Uptane::Target::Unknown()};
  std::string update_reason{"unknown"};

  switch (update_type) {
    case UpdateType::kNewTargetUpdate: {
      update_reason = "Auto update from " + current_target.filename() + " to " + desired_target.filename();
      update_target = Target::subtractCurrentApps(desired_target, current_target, app_shortlist_);
      break;
    }
    case UpdateType::kCurrentTargetSync: {
      update_reason = "Sync current Target " + current_target.filename();
      update_target = Target::subtractCurrentApps(desired_target, current_target, app_shortlist_);
      break;
    }
    case UpdateType::kTargetForcedUpdate: {
      update_reason = "Manual/Forced update to Target " + desired_target.filename();
      update_target = desired_target;
      break;
    }
    default: {
      // this shouldn't really happen at the runtime
      throw std::runtime_error("Unsupported update type:  " + std::to_string(update_type));
    }
  }
  // set an update correlation ID to "bind" all report events generated during a single update together
  // so the backend can know to which of the updates specific events are related to
  Target::setCorrelationID(update_target);
  return {update_reason, update_target};
}

void LiteClient::logUpdate(UpdateType update_type, const Uptane::Target& desired_target,
                           const Uptane::Target& current_target) const {
  switch (update_type) {
    case UpdateType::kNewTargetUpdate: {
      logTarget("Updating Active Target: ", current_target);
      logTarget("To New Target: ", desired_target);
      break;
    }
    case UpdateType::kCurrentTargetSync: {
      logTarget("Syncing current Target: ", desired_target);
      break;
    }
    case UpdateType::kTargetForcedUpdate: {
      logTarget("Updating to Target: ", desired_target);
      break;
    }
    default: {
      // this shouldn't really happen at the runtime
      throw std::runtime_error("Unsupported update type:  " + std::to_string(update_type));
    }
  }
}

data::ResultCode::Numeric LiteClient::doUpdate(const Uptane::Target& desired_target,
                                               const Uptane::Target& update_target, const std::string& update_reason) {
  data::ResultCode::Numeric update_result = data::ResultCode::Numeric::kUnknown;
  update_result = download(update_target, update_reason);

  if (update_result != data::ResultCode::Numeric::kOk) {
    return update_result;
  }
  update_result = install(desired_target, update_target);
  prune(desired_target);
  return update_result;
}

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

std::unique_ptr<Lock> LiteClient::getDownloadLock() const { return create_lock(download_lockfile_); }

data::ResultCode::Numeric LiteClient::install(const Uptane::Target& desired_target,
                                              const Uptane::Target& update_target) {
  std::unique_ptr<Lock> lock = getUpdateLock();
  if (lock == nullptr) {
    return data::ResultCode::Numeric::kInternalError;
  }

  notifyInstallStarted(desired_target);
  auto iresult = installPackage(update_target);
  if (iresult.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
    LOG_INFO << "Update complete. Please reboot the device to activate";
    storage_->savePrimaryInstalledVersion(desired_target, InstalledVersionUpdateMode::kPending);
    is_reboot_required_ = booted_sysroot;
  } else if (iresult.result_code.num_code == data::ResultCode::Numeric::kOk) {
    LOG_INFO << "Update complete. No reboot needed";
    storage_->savePrimaryInstalledVersion(desired_target, InstalledVersionUpdateMode::kCurrent);
  } else if (iresult.result_code.num_code == data::ResultCode::Numeric::kAlreadyProcessed) {
    LOG_INFO << "Device is already in sync with the given Target: " << desired_target.filename();
    storage_->savePrimaryInstalledVersion(desired_target, InstalledVersionUpdateMode::kCurrent);
  } else {
    LOG_ERROR << "Unable to install update: " << iresult.description;
    // let go of the lock since we couldn't update
  }
  notifyInstallFinished(desired_target, iresult);
  return iresult.result_code.num_code;
}

data::InstallationResult LiteClient::installPackage(const Uptane::Target& target) {
  LOG_INFO << "Installing package using " << package_manager_->name() << " package manager";
  try {
    return package_manager_->install(target);
  } catch (std::exception& ex) {
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, ex.what());
  }
}

void LiteClient::prune(const Uptane::Target& target) {
  if (package_manager_->name() == PACKAGE_MANAGER_OSTREE) {
    return;
  }
  assert(package_manager_->name() == ComposeAppManager::Name);

  auto* compose_app_manager = dynamic_cast<ComposeAppManager*>(package_manager_.get());
  assert(compose_app_manager != nullptr);

  compose_app_manager->handleRemovedApps(target);
}

void LiteClient::writeCurrentTarget() const {
  const Uptane::Target& t = getCurrent();
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
  Utils::writeFile(config_.storage.path / "current-target", ss.str());
}

std::unique_ptr<Lock> LiteClient::getUpdateLock() const { return create_lock(update_lockfile_); }

class DetailedDownloadReport : public EcuDownloadStartedReport {
 public:
  DetailedDownloadReport(const Uptane::EcuSerial& ecu, const std::string& correlation_id, const std::string& details)
      : EcuDownloadStartedReport(ecu, correlation_id) {
    custom["details"] = details;
  }
};

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

void LiteClient::notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir) {
  if (ir.needCompletion()) {
    callback("install-post", t, "NEEDS_COMPLETION");
    notify(t, std_::make_unique<DetailedAppliedReport>(primary_ecu_.first, t.correlation_id(), ir.description));
    return;
  }

  if (ir.result_code == data::ResultCode::Numeric::kOk) {
    callback("install-post", t, "OK");
    writeCurrentTarget();
    notify(t, std_::make_unique<DetailedInstallCompletedReport>(primary_ecu_.first, t.correlation_id(), true,
                                                                ir.description));
  } else {
    callback("install-post", t, "FAILED");
    notify(t, std_::make_unique<DetailedInstallCompletedReport>(primary_ecu_.first, t.correlation_id(), false,
                                                                ir.description));
  }
}

void LiteClient::notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event) {
  if (!config_.tls.server.empty()) {
    event->custom["targetName"] = t.filename();
    event->custom["version"] = t.custom_version();
    report_queue_->enqueue(std::move(event));
  }
}

void LiteClient::callback(const char* msg, const Uptane::Target& install_target, const std::string& result) {
  if (callback_program_.empty()) {
    return;
  }
  auto env = boost::this_process::environment();
  boost::process::environment env_copy = env;
  env_copy["MESSAGE"] = msg;
  env_copy["CURRENT_TARGET"] = (config_.storage.path / "current-target").string();

  if (!install_target.MatchTarget(Uptane::Target::Unknown())) {
    env_copy["INSTALL_TARGET"] = install_target.filename();
  }
  if (!result.empty()) {
    env_copy["RESULT"] = result;
  }

  int rc = boost::process::system(callback_program_, env_copy);
  if (rc != 0) {
    LOG_ERROR << "Error with callback: " << rc;
  }
}

void LiteClient::addAppsHeader(std::vector<std::string>& headers, PackageConfig& config) {
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

void LiteClient::updateRequestHeaders() {
  const auto target = getCurrent();
  http_client_->updateHeader("x-ats-target", target.filename());

  if (config_.pacman.type == ComposeAppManager::Name) {
    ComposeAppManager::Config cfg(config_.pacman);

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
      http_client_->updateHeader("x-ats-dockerapps", boost::algorithm::join(apps, ","));
    }
  }
}

void LiteClient::setInvalidTargets() {
  invalid_targets_.clear();

  std::vector<Uptane::Target> known_versions;
  storage_->loadPrimaryInstallationLog(&known_versions, false);

  std::vector<Uptane::Target> installed_versions;
  storage_->loadPrimaryInstallationLog(&installed_versions, true);

  for (const auto& t : known_versions) {
    if (installed_versions.end() ==
        std::find_if(installed_versions.begin(), installed_versions.end(),
                     [&t](const Uptane::Target& t1) { return t.filename() == t1.filename(); })) {
      // known but never successfully installed version
      invalid_targets_.push_back(t);
    }
  }
}
