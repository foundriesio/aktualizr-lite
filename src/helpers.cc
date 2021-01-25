#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>

#include <boost/process.hpp>
#include <boost/process/environment.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "composeappmanager.h"
#include "helpers.h"
#include "ostree.h"
#include "package_manager/ostreemanager.h"

void log_info_target(const std::string& prefix, const Config& config, const Uptane::Target& t) {
  auto name = t.filename();
  if (t.custom_version().length() > 0) {
    name = t.custom_version();
  }
  LOG_INFO << prefix + name << "\tsha256:" << t.sha256Hash();

  if (config.pacman.type == ComposeAppManager::Name) {
    bool shown = false;
    auto config_apps = ComposeAppManager::Config(config.pacman).apps;
    auto bundles = t.custom_data()["docker_compose_apps"];
    for (Json::ValueIterator i = bundles.begin(); i != bundles.end(); ++i) {
      if (!shown) {
        shown = true;
        LOG_INFO << "\tDocker Compose Apps:";
      }
      if ((*i).isObject() && (*i).isMember("uri")) {
        const auto& app = i.key().asString();
        std::string app_status =
            (!config_apps || (*config_apps).end() != std::find((*config_apps).begin(), (*config_apps).end(), app))
                ? "on"
                : "off";
        LOG_INFO << "\t" << app_status << ": " << app << " -> " << (*i)["uri"].asString();
      } else {
        LOG_ERROR << "\t\tInvalid custom data for docker_compose_apps: " << i.key().asString();
      }
    }
  }
}

static void add_apps_header(std::vector<std::string>& headers, PackageConfig& config) {
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

static void update_request_headers(std::shared_ptr<HttpClient>& http_client, const Uptane::Target& target,
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

static bool appListChanged(const Json::Value& target_apps, std::vector<std::string>& cfg_apps_in,
                           const boost::filesystem::path& apps_dir) {
  // Did the list of installed versus running apps change:
  std::vector<std::string> found;
  if (boost::filesystem::is_directory(apps_dir)) {
    for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(apps_dir), {})) {
      if (boost::filesystem::is_directory(entry)) {
        found.emplace_back(entry.path().filename().native());
      }
    }
  }
  // don't take into consideration the apps that are listed in the config but are not present in Target
  // do take into consideration the apps that are found on a file system but are not present in Target
  // since they should be removed, so we need to trigger the installation procedure that will remove them
  auto cfg_apps_filtered_end = cfg_apps_in.end();
  if (!target_apps.isNull()) {
    cfg_apps_filtered_end =
        std::remove_if(cfg_apps_in.begin(), cfg_apps_in.end(),
                       [&target_apps](const std::string& app) { return !target_apps.isMember(app.c_str()); });
  }
  std::vector<std::string> cfg_apps{cfg_apps_in.begin(), cfg_apps_filtered_end};
  std::sort(found.begin(), found.end());
  std::sort(cfg_apps.begin(), cfg_apps.end());
  if (found != cfg_apps) {
    LOG_INFO << "Config change detected: list of apps has changed";
    return true;
  }
  return false;
}

bool LiteClient::composeAppsChanged() const {
  if (config.pacman.type == ComposeAppManager::Name) {
    ComposeAppManager::Config cacfg(config.pacman);
    if (!cacfg.apps) {
      // `compose_apps` is not specified in the config at all
      return false;
    }
    if (appListChanged(getCurrent().custom_data()["docker_compose_apps"], *cacfg.apps, cacfg.apps_root)) {
      return true;
    }

  } else {
    return false;
  }

  return false;
}

static std::tuple<Uptane::Target, Uptane::Target, data::InstallationResult> finalizeIfNeeded(OSTree::Sysroot& sysroot,
                                                                                             INvStorage& storage,
                                                                                             Config& config) {
  data::InstallationResult ir{data::ResultCode::Numeric::kUnknown, ""};
  boost::optional<Uptane::Target> pending_version;
  boost::optional<Uptane::Target> current_version;
  storage.loadInstalledVersions("", &current_version, &pending_version);

  std::string current_hash = sysroot.getCurDeploymentHash();
  if (current_hash.empty()) {
    throw std::runtime_error("Could not get " + sysroot.type() + " deployment in " + sysroot.path());
  }

  Bootloader bootloader(config.bootloader, storage);

  if (!!pending_version) {
    const Uptane::Target& target = *pending_version;
    if (current_hash == target.sha256Hash()) {
      LOG_INFO << "Marking target install complete for: " << target;
      storage.saveInstalledVersion("", target, InstalledVersionUpdateMode::kCurrent);
      ir.result_code = data::ResultCode::Numeric::kOk;
      if (bootloader.rebootDetected()) {
        bootloader.rebootFlagClear();
      }
      // Installation was successful, so both currently installed Target and Target that has been applied are the same
      return std::tie(target, target, ir);
    } else {
      if (bootloader.rebootDetected()) {
        std::string err = "Expected to boot on " + target.sha256Hash() + " buf found " + current_hash +
                          ", system might have experienced a rollback";
        LOG_ERROR << err;
        storage.saveInstalledVersion("", target, InstalledVersionUpdateMode::kNone);
        bootloader.rebootFlagClear();
        ir.result_code = data::ResultCode::Numeric::kInstallFailed;
        ir.description = err;
      } else {
        // Update still pending as no reboot was detected
        ir.result_code = data::ResultCode::Numeric::kNeedCompletion;
      }
      // Installation was not successful
      return std::tie(*current_version, target, ir);
    }
  }

  std::vector<Uptane::Target> installed_versions;
  storage.loadPrimaryInstallationLog(&installed_versions, false);

  // Version should be in installed versions. Its possible that multiple
  // targets could have the same sha256Hash. In this case the safest assumption
  // is that the most recent (the reverse of the vector) target is what we
  // should return.
  std::vector<Uptane::Target>::reverse_iterator it;
  for (it = installed_versions.rbegin(); it != installed_versions.rend(); it++) {
    if (it->sha256Hash() == current_hash) {
      ir.result_code = data::ResultCode::Numeric::kAlreadyProcessed;
      return std::tie(*it, *it, ir);
    }
  }
  Uptane::Target unknown_target{Uptane::Target::Unknown()};
  return std::tie(unknown_target, unknown_target, ir);
}

LiteClient::LiteClient(Config& config_in)
    : config{std::move(config_in)}, primary_ecu{Uptane::EcuSerial::Unknown(), ""} {
  std::string pkey;
  storage = INvStorage::newStorage(config.storage);
  storage->importData(config.import);

  const std::map<std::string, std::string> raw = config.pacman.extra;
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
    headers.emplace_back("x-ats-primary: " + primary_ecu.first.ToString());
  }

  headers.emplace_back("x-ats-tags: " + boost::algorithm::join(tags, ","));

  http_client = std::make_shared<HttpClient>(&headers);
  uptane_fetcher_ = std::make_shared<Uptane::Fetcher>(config, http_client);
  report_queue = std_::make_unique<ReportQueue>(config, http_client, storage);

  // finalizeIfNeeded it looks like copy-paste of SotaUptaneClient::finalizeAfterReboot
  // can we use just SotaUptaneClient::finalizeAfterReboot or even maybe SotaUptaneClient::initialize ???
  // in this case we could do our specific finalization, including starting apps, in ComposeAppManager::finalizeInstall
  Uptane::Target current_target{Uptane::Target::Unknown()};
  Uptane::Target target_been_applied{Uptane::Target::Unknown()};
  data::InstallationResult target_installation_result;
  std::tie(current_target, target_been_applied, target_installation_result) =
      finalizeIfNeeded(*ostree_sysroot, *storage, config);
  update_request_headers(http_client, current_target, config.pacman);

  key_manager_ = std_::make_unique<KeyManager>(storage, config.keymanagerConfig());
  key_manager_->loadKeys();
  key_manager_->copyCertsToCurl(*http_client);

  // TODO: consider improving this factory method
  if (config.pacman.type == ComposeAppManager::Name) {
    package_manager_ =
        std::make_shared<ComposeAppManager>(config.pacman, config.bootloader, storage, http_client, ostree_sysroot);
  } else if (config.pacman.type == PACKAGE_MANAGER_OSTREE) {
    package_manager_ = std::make_shared<OstreeManager>(config.pacman, config.bootloader, storage, http_client);
  } else {
    throw std::runtime_error("Unsupported package manager type: " + config.pacman.type);
  }

  writeCurrentTarget(current_target);
  if (target_installation_result.result_code != data::ResultCode::Numeric::kAlreadyProcessed) {
    notifyInstallFinished(target_been_applied, target_installation_result);
  }
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

bool LiteClient::checkForUpdates() {
  Uptane::Target t = Uptane::Target::Unknown();
  callback("check-for-update-pre", t);
  bool rc = updateImageMeta();
  callback("check-for-update-post", t);
  return rc;
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
  notify(t, std_::make_unique<DetailedDownloadReport>(primary_ecu.first, t.correlation_id(), reason));
}

void LiteClient::notifyDownloadFinished(const Uptane::Target& t, bool success) {
  callback("download-post", t, success ? "OK" : "FAILED");
  notify(t, std_::make_unique<EcuDownloadCompletedReport>(primary_ecu.first, t.correlation_id(), success));
}

void LiteClient::notifyInstallStarted(const Uptane::Target& t) {
  callback("install-pre", t);
  notify(t, std_::make_unique<EcuInstallationStartedReport>(primary_ecu.first, t.correlation_id()));
}

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
    notify(t, std_::make_unique<EcuInstallationAppliedReport>(primary_ecu.first, t.correlation_id()));
  } else if (ir.result_code == data::ResultCode::Numeric::kOk) {
    callback("install-post", t, "OK");
    writeCurrentTarget(t);
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

void generate_correlation_id(Uptane::Target& t) {
  std::string id = t.custom_version();
  if (id.empty()) {
    id = t.filename();
  }
  boost::uuids::uuid tmp = boost::uuids::random_generator()();
  t.setCorrelationId(id + "-" + boost::uuids::to_string(tmp));
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

bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags) {
  if (!config_tags.empty()) {
    auto tags = t.custom_data()["tags"];
    for (Json::ValueIterator i = tags.begin(); i != tags.end(); ++i) {
      auto tag = (*i).asString();
      if (std::find(config_tags.begin(), config_tags.end(), tag) != config_tags.end()) {
        return true;
      }
    }
    return false;
  }
  return true;
}

bool LiteClient::isTargetCurrent(const Uptane::Target& target) const {
  if (!targets_eq(target, getCurrent(), true)) {
    return false;
  }

  if (package_manager_->name() == ComposeAppManager::Name) {
    auto* compose_pacman = dynamic_cast<ComposeAppManager*>(package_manager_.get());
    if (compose_pacman == nullptr) {
      LOG_ERROR << "Cannot downcast the package manager to a specific type";
      return false;
    }

    // Deamon Update Cycle/Loop, do non-full check if Target Apps are installed and running
    return compose_pacman->checkForAppsToUpdate(target, boost::none);
  }

  return true;
}

bool LiteClient::checkAppsToUpdate(const Uptane::Target& target) const {
  if (package_manager_->name() == ComposeAppManager::Name) {
    auto* compose_pacman = dynamic_cast<ComposeAppManager*>(package_manager_.get());
    if (compose_pacman == nullptr) {
      LOG_ERROR << "Cannot downcast the package manager to a specific type";
      return false;
    }
    // first Update Cycle/Loop, do full check if Target Apps are installed and running
    LOG_INFO << "Checking for Apps to be installed or updated...";
    return compose_pacman->checkForAppsToUpdate(target, true);
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

// TODO: this has to be refactored: target comparision should be in the package manager context
// at least the logic that gets a list of apps and apps root folder since they are package manager specific.

bool targets_eq(const Uptane::Target& t1, const Uptane::Target& t2, bool compareApps) {
  if (!match_target_base(t1, t2)) {
    return false;
  }

  if (!compareApps) {
    return true;
  }

  // compose apps
  // We are checking if Apps that are supposed to be installed (listed in the currenlty installed Target
  // are actually present on a system. Apps are installed on non read-only mount point/folder so could be
  // modified/removed somehow so we need to return 'false' here and let aklite update/re-install App(s) It's
  // workaround, a proper solution for 'immutable Target' concept is installing Apps on read-only system (both
  // meta-data and actuall container image layers)
  auto t1_capps = t1.custom_data()["docker_compose_apps"];
  auto t2_capps = t2.custom_data()["docker_compose_apps"];
  for (Json::ValueIterator i = t1_capps.begin(); i != t1_capps.end(); ++i) {
    auto app = i.key().asString();
    if (!t2_capps.isMember(app)) {
      return false;  // an app has been removed
    }
    if ((*i)["uri"].asString() != t2_capps[app]["uri"].asString()) {
      return false;  // tuf target filename changed
    }

    t2_capps.removeMember(app);
  }

  return t2_capps.empty();
}

bool known_local_target(LiteClient& client, const Uptane::Target& t, std::vector<Uptane::Target>& installed_versions) {
  bool known_target = false;
  auto current = client.getCurrent();
  boost::optional<Uptane::Target> pending;
  client.storage->loadPrimaryInstalledVersions(nullptr, &pending);

  if (t.sha256Hash() != current.sha256Hash()) {
    std::vector<Uptane::Target>::reverse_iterator it;
    for (it = installed_versions.rbegin(); it != installed_versions.rend(); it++) {
      if (it->sha256Hash() == t.sha256Hash()) {
        // Make sure installed version is not what is currently pending
        if ((pending != boost::none) && (it->sha256Hash() == pending->sha256Hash())) {
          continue;
        }
        LOG_INFO << "Target sha256Hash " << t.sha256Hash() << " known locally (rollback?), skipping";
        known_target = true;
        break;
      }
    }
  }
  return known_target;
}

bool match_target_base(const Uptane::Target& t1, const Uptane::Target& t2) {
  if (t1.type() != t2.type() || t2.type() != "OSTREE") {
    // both targets' type should be OSTREE, otherwise it's error
    // but we don't throw an exception, just log an error message and let the update loop
    // to do its work in a hope that the backend will send us a proper Target
    LOG_ERROR << "Target formats mismatch: " << t1.type() << " != " << t2.type();
    return false;
  }

  if (t1.length() != t2.length()) {
    // both targets' type should be OSTREE, as well as their lengths should be zero, otherwise it's error
    // but we don't throw an exception, just log an error message and let the update loop
    // to do its work in a hope that the backend will send us a proper Target
    LOG_ERROR << "Target lengths differ: " << t1.length() << " != " << t2.length();
    return false;
  }

  if (t1.filename() != t2.filename()) {
    // Any changes in Target means Target name/ID/version change, so this is a valid situation
    // and  means that we need to proceed with Target update
    LOG_INFO << "Target names differ " << t1.filename() << " != " << t2.filename();
    return false;
  }

  if (t1.sha256Hash() != t2.sha256Hash()) {
    // if 'filename' (aka target number/ID/version) are the same then the hashes are supposed to be the same too,
    // so this is an error situation
    LOG_ERROR << "Target hashes differ " << t1.sha256Hash() << " != " << t2.sha256Hash();
    return false;
  }

  return true;
}
