#include <sys/file.h>
#include <unistd.h>
#include <fcntl.h>
#include <unistd.h>

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "helpers.h"
#include "package_manager/ostreemanager.h"
#include "package_manager/packagemanagerfactory.h"

#ifdef BUILD_DOCKERAPP
#include "package_manager/dockerappmanager.h"
static void add_apps_header(std::vector<std::string> &headers, PackageConfig &config) {
  if (config.type == PACKAGE_MANAGER_OSTREEDOCKERAPP) {
    DockerAppManagerConfig dappcfg(config);
    headers.emplace_back("x-ats-dockerapps: " + boost::algorithm::join(dappcfg.docker_apps, ","));
  }
}
bool should_compare_docker_apps(const Config &config) {
  DockerAppManagerConfig dappcfg(config.pacman);
  return (config.pacman.type == PACKAGE_MANAGER_OSTREEDOCKERAPP && !dappcfg.docker_apps.empty());
}

void LiteClient::storeDockerParamsDigest() {
  DockerAppManagerConfig dappcfg(config.pacman);
  auto digest = config.storage.path / ".params-hash";
  if (boost::filesystem::exists(dappcfg.docker_app_params)) {
    Utils::writeFile(digest, Crypto::sha256digest(Utils::readFile(dappcfg.docker_app_params)));
  } else {
    unlink(digest.c_str());
  }
}

bool LiteClient::dockerAppsChanged() {
  if (config.pacman.type != PACKAGE_MANAGER_OSTREEDOCKERAPP) {
    return false;
  }

  DockerAppManagerConfig dappcfg(config.pacman);
  // Did the list of installed versus running apps change:
  std::vector<std::string> found;
  if (boost::filesystem::is_directory(dappcfg.docker_apps_root)) {
    for (auto &entry :
         boost::make_iterator_range(boost::filesystem::directory_iterator(dappcfg.docker_apps_root), {})) {
      if (boost::filesystem::is_directory(entry)) {
        found.emplace_back(entry.path().filename().native());
      }
    }
  }
  std::sort(found.begin(), found.end());
  std::sort(dappcfg.docker_apps.begin(), dappcfg.docker_apps.end());
  if (found != dappcfg.docker_apps) {
    LOG_INFO << "Config change detected: list of apps has changed";
    return true;
  }

  // Did the docker app configuration change
  auto checksum = config.storage.path / ".params-hash";
  if (boost::filesystem::exists(dappcfg.docker_app_params)) {
    if (dappcfg.docker_apps.size() == 0) {
      // there's no point checking for changes - nothing is running
      return false;
    }

    if (boost::filesystem::exists(checksum)) {
      std::string cur = Utils::readFile(checksum);
      std::string now = Crypto::sha256digest(Utils::readFile(dappcfg.docker_app_params));
      if (cur != now) {
        LOG_INFO << "Config change detected: docker-app-params content has changed";
        return true;
      }
    } else {
      LOG_INFO << "Config change detected: docker-app-params have been defined";
      return true;
    }
  } else if (boost::filesystem::exists(checksum)) {
    LOG_INFO << "Config change detected: docker-app parameters have been removed";
    return true;
  }

  return false;
}
#else /* ! BUILD_DOCKERAPP */
#define add_apps_header(headers, config) \
  {}

bool should_compare_docker_apps(const Config &config) {
  (void)config;
  return false;
}

void LiteClient::storeDockerParamsDigest() {}
bool LiteClient::dockerAppsChanged() { return false; }
#endif

static std::pair<Uptane::Target, data::ResultCode::Numeric> finalizeIfNeeded(PackageManagerInterface &package_manager,
                                                                             INvStorage &storage, Config &config) {
  data::ResultCode::Numeric result_code = data::ResultCode::Numeric::kUnknown;
  boost::optional<Uptane::Target> pending_version;
  storage.loadInstalledVersions("", nullptr, &pending_version);

  GObjectUniquePtr<OstreeSysroot> sysroot_smart = OstreeManager::LoadSysroot(config.pacman.sysroot);
  OstreeDeployment *booted_deployment = ostree_sysroot_get_booted_deployment(sysroot_smart.get());
  std::string current_hash = ostree_deployment_get_csum(booted_deployment);
  if (booted_deployment == nullptr) {
    throw std::runtime_error("Could not get booted deployment in " + config.pacman.sysroot.string());
  }

  Bootloader bootloader(config.bootloader, storage);

  if (!!pending_version) {
    const Uptane::Target &target = *pending_version;
    if (current_hash == target.sha256Hash()) {
      LOG_INFO << "Marking target install complete for: " << target;
      storage.saveInstalledVersion("", target, InstalledVersionUpdateMode::kCurrent);
      result_code = data::ResultCode::Numeric::kOk;
      if (bootloader.rebootDetected()) {
        bootloader.rebootFlagClear();
      }
    } else {
      if (bootloader.rebootDetected()) {
        LOG_ERROR << "Expected to boot on " << target.sha256Hash() << " but found " << current_hash
                  << ", system might have experienced a rollback";
        storage.saveInstalledVersion("", target, InstalledVersionUpdateMode::kNone);
        bootloader.rebootFlagClear();
        result_code = data::ResultCode::Numeric::kInstallFailed;
      } else {
        // Update still pending as no reboot was detected
        result_code = data::ResultCode::Numeric::kNeedCompletion;
      }
    }
    return std::make_pair(target, result_code);
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
      return std::make_pair(*it, data::ResultCode::Numeric::kAlreadyProcessed);
    }
  }
  return std::make_pair(Uptane::Target::Unknown(), result_code);
}

LiteClient::LiteClient(Config &config_in)
    : config(std::move(config_in)), primary_ecu(Uptane::EcuSerial::Unknown(), "") {
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
  GObjectUniquePtr<OstreeSysroot> sysroot_smart = OstreeManager::LoadSysroot(config.pacman.sysroot);
  OstreeDeployment *deployment = ostree_sysroot_get_booted_deployment(sysroot_smart.get());
  std::string header("x-ats-ostreehash: ");
  if (deployment != nullptr) {
    header += ostree_deployment_get_csum(deployment);
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
  report_queue = std_::make_unique<ReportQueue>(config, http_client);
  package_manager = PackageManagerFactory::makePackageManager(config.pacman, config.bootloader, storage, http_client);

  std::pair<Uptane::Target, data::ResultCode::Numeric> pair = finalizeIfNeeded(*package_manager, *storage, config);
  http_client->updateHeader("x-ats-target", pair.first.filename());

  KeyManager keys(storage, config.keymanagerConfig());
  keys.copyCertsToCurl(*http_client);

  primary =
      std::make_shared<SotaUptaneClient>(config, storage, http_client, nullptr, primary_ecu.first, primary_ecu.second);

  writeCurrentTarget(pair.first);
  if (pair.second != data::ResultCode::Numeric::kAlreadyProcessed) {
    notifyInstallFinished(pair.first, pair.second);
  }
}

void LiteClient::notify(const Uptane::Target &t, std::unique_ptr<ReportEvent> event) {
  if (!config.tls.server.empty()) {
    event->custom["targetName"] = t.filename();
    event->custom["version"] = t.custom_version();
    report_queue->enqueue(std::move(event));
  }
}

void LiteClient::notifyDownloadStarted(const Uptane::Target &t) {
  notify(t, std_::make_unique<EcuDownloadStartedReport>(primary_ecu.first, t.correlation_id()));
}

void LiteClient::notifyDownloadFinished(const Uptane::Target &t, bool success) {
  notify(t, std_::make_unique<EcuDownloadCompletedReport>(primary_ecu.first, t.correlation_id(), success));
}

void LiteClient::notifyInstallStarted(const Uptane::Target &t) {
  notify(t, std_::make_unique<EcuInstallationStartedReport>(primary_ecu.first, t.correlation_id()));
}

void LiteClient::notifyInstallFinished(const Uptane::Target &t, data::ResultCode::Numeric rc) {
  if (rc == data::ResultCode::Numeric::kNeedCompletion) {
    notify(t, std_::make_unique<EcuInstallationAppliedReport>(primary_ecu.first, t.correlation_id()));
  } else if (rc == data::ResultCode::Numeric::kOk) {
    writeCurrentTarget(t);
    notify(t, std_::make_unique<EcuInstallationCompletedReport>(primary_ecu.first, t.correlation_id(), true));
  } else {
    notify(t, std_::make_unique<EcuInstallationCompletedReport>(primary_ecu.first, t.correlation_id(), false));
  }
}

void LiteClient::writeCurrentTarget(const Uptane::Target &t) {
  std::stringstream ss;
  ss << "TARGET_NAME=\"" << t.filename() << "\"\n";
  ss << "CUSTOM_VERSION=\"" << t.custom_version() << "\"\n";
  Json::Value custom = t.custom_data();
  std::string tmp = custom["lmp-manifest-sha"].asString();
  if (tmp.size() > 0) {
    ss << "LMP_MANIFEST_SHA=\"" << tmp << "\"\n";
  }
  tmp = custom["meta-subscriber-overrides-sha"].asString();
  if (tmp.size() > 0) {
    ss << "META_SUBSCRIBER_OVERRIDES_SHA=\"" << tmp << "\"\n";
  }
  tmp = custom["containers-sha"].asString();
  if (tmp.size() > 0) {
    ss << "CONTAINERS_SHA=\"" << tmp << "\"\n";
  }
  Utils::writeFile(config.storage.path / "current-target", ss.str());
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

std::unique_ptr<Lock> LiteClient::getDownloadLock() { return create_lock(download_lockfile); }
std::unique_ptr<Lock> LiteClient::getUpdateLock() { return create_lock(update_lockfile); }

void generate_correlation_id(Uptane::Target &t) {
  std::string id = t.custom_version();
  if (id.empty()) {
    id = t.filename();
  }
  boost::uuids::uuid tmp = boost::uuids::random_generator()();
  t.setCorrelationId(id + "-" + boost::uuids::to_string(tmp));
}

bool target_has_tags(const Uptane::Target &t, const std::vector<std::string> &config_tags) {
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

bool targets_eq(const Uptane::Target &t1, const Uptane::Target &t2, bool compareDockerApps) {
  // target equality check looks at hashes
  if (t1.MatchTarget(t2)) {
    if (compareDockerApps) {
      auto t1_apps = t1.custom_data()["docker_apps"];
      auto t2_apps = t2.custom_data()["docker_apps"];
      for (Json::ValueIterator i = t1_apps.begin(); i != t1_apps.end(); ++i) {
        auto app = i.key().asString();
        if (!t2_apps.isMember(app)) {
          return false;  // an app has been removed
        }
        if ((*i)["filename"].asString() != t2_apps[app]["filename"].asString()) {
          return false;  // tuf target filename changed
        }
        t2_apps.removeMember(app);
      }
      if (t2_apps.size() > 0) {
        return false;  // an app has been added
      }
    }
    return true;  // docker apps are the same, or there are none
  }
  return false;
}

bool known_local_target(LiteClient &client, const Uptane::Target &t, std::vector<Uptane::Target> &installed_versions) {
  bool known_target = false;
  auto current = client.primary->getCurrent();
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
