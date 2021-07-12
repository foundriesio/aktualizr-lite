#include "liteclient.h"

#include <fcntl.h>
#include <sys/file.h>

#include <boost/process.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "composeappmanager.h"
#include "crypto/keymanager.h"
#include "target.h"

LiteClient::LiteClient(Config& config_in, const AppEngine::Ptr& app_engine, const std::shared_ptr<P11EngineGuard>& p11)
    : config{std::move(config_in)}, primary_ecu{Uptane::EcuSerial::Unknown(), ""} {
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
  headers.emplace_back("x-ats-target: unknown");
  add_apps_header(headers, config.pacman);

  if (!config.telemetry.report_network) {
    // Provide the random primary ECU serial so our backend will have some
    // idea of the number of unique devices using the system
    headers.emplace_back("x-ats-primary: " + primary_ecu.first.ToString());
  }

  headers.emplace_back("x-ats-tags: " + boost::algorithm::join(tags, ","));

  http_client = std::make_shared<HttpClient>(&headers);

  key_manager_ = std_::make_unique<KeyManager>(storage, config.keymanagerConfig(), p11);
  key_manager_->loadKeys();
  key_manager_->copyCertsToCurl(*http_client);

  uptane_fetcher_ = std::make_shared<Uptane::Fetcher>(config, http_client);
  report_queue = std_::make_unique<ReportQueue>(config, http_client, storage);

  if (config.pacman.type == ComposeAppManager::Name) {
    package_manager_ = std::make_shared<ComposeAppManager>(config.pacman, config.bootloader, storage, http_client,
                                                           ostree_sysroot, app_engine);
  } else if (config.pacman.type == PACKAGE_MANAGER_OSTREE) {
    package_manager_ = std::make_shared<OstreeManager>(config.pacman, config.bootloader, storage, http_client);
  } else {
    throw std::runtime_error("Unsupported package manager type: " + config.pacman.type);
  }
}

data::InstallationResult LiteClient::finalizePendingUpdate(boost::optional<Uptane::Target>& target) {
  data::InstallationResult ret{data::ResultCode::Numeric::kNeedCompletion, ""};
  InstalledVersionUpdateMode mode = InstalledVersionUpdateMode::kPending;
  bool rollback = false;
  LOG_INFO << "Finalize Pending, Target: " << target->filename() << ", hash: " << target->sha256Hash();

  ret = package_manager_->finalizeInstall(*target);
  if (ret.isSuccess()) {
    LOG_INFO << "Finalize Complete, Target: " << target->filename();
    mode = InstalledVersionUpdateMode::kCurrent;
  } else {
    rollback = data::ResultCode::Numeric::kInstallFailed == ret.result_code.num_code;
    // update dB Target record clearing flags (is_pending, was_installed)
    if (rollback) {
      LOG_ERROR << "Finalize Failed (rollback happened), Target: " << target->filename();
      mode = InstalledVersionUpdateMode::kNone;
    }
  }
  if (ret.isSuccess() || rollback) {
    storage->saveInstalledVersion("", *target, mode);
  }
  return ret;
}

bool LiteClient::finalizeInstall() {
  data::InstallationResult ret{data::ResultCode::Numeric::kNeedCompletion, ""};
  boost::optional<Uptane::Target> pending;

  // finalize pending installs
  storage->loadInstalledVersions("", nullptr, &pending);
  if (!!pending) {
    ret = finalizePendingUpdate(pending);
  } else {
    LOG_INFO << "No Pending Installs";
  }

  // write current to /var/sota
  const auto current = getCurrent();
  update_request_headers(http_client, current, config.pacman);
  writeCurrentTarget(current);

  // notify the backend
  if (!ret.needCompletion()) {
    notifyInstallFinished(*pending, ret);
  }

  return ret.isSuccess();
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
  Uptane::Target t = Uptane::Target::Unknown();
  callback("check-for-update-pre", t);
  const auto rc = updateImageMeta();
  if (!std::get<0>(rc)) {
    callback("check-for-update-post", t, "FAILED: " + std::get<1>(rc));
  }
  return std::get<0>(rc);
}

void LiteClient::checkForUpdatesEnd(const Uptane::Target& target) { callback("check-for-update-post", target, "OK"); }

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
    notify(t, std_::make_unique<DetailedAppliedReport>(primary_ecu.first, t.correlation_id(), ir.description));
    return;
  }

  if (ir.result_code == data::ResultCode::Numeric::kOk) {
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

data::InstallationResult LiteClient::installPackage(const Uptane::Target& target) {
  LOG_INFO << "Installing package using " << package_manager_->name() << " package manager";
  try {
    return package_manager_->install(target);
  } catch (std::exception& ex) {
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, ex.what());
  }
}

std::tuple<bool, std::string> LiteClient::updateImageMeta() {
  try {
    image_repo_.updateMeta(*storage, *uptane_fetcher_);
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to update Image repo metadata: " << e.what();
    return {false, e.what()};
  }

  return {true, ""};
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

data::ResultCode::Numeric LiteClient::download(const Uptane::Target& target, const std::string& reason) {
  notifyDownloadStarted(target, reason);
  if (!downloadImage(target).first) {
    notifyDownloadFinished(target, false);
    return data::ResultCode::Numeric::kDownloadFailed;
  }
  notifyDownloadFinished(target, true);
  return data::ResultCode::Numeric::kOk;
}

data::ResultCode::Numeric LiteClient::install(const Uptane::Target& target) {
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
    http_client->updateHeader("x-ats-dockerapps", Target::appsStr(target, ComposeAppManager::Config(config).apps));
  }
}

void LiteClient::logTarget(const std::string& prefix, const Uptane::Target& target) const {
  Target::log(
      prefix, target,
      config.pacman.type == ComposeAppManager::Name ? ComposeAppManager::Config(config.pacman).apps : boost::none);
}
