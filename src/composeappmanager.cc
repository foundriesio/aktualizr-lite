#include "composeappmanager.h"

#include <set>

#include <boost/format.hpp>
#include <boost/process.hpp>
#include <boost/range/iterator_range_core.hpp>

#include "bootloader/bootloaderlite.h"
#include "docker/restorableappengine.h"
#include "target.h"
#ifdef USE_COMPOSEAPP_ENGINE
#include "composeapp/appengine.h"
#endif  // USE_COMPOSEAPP_ENGINE

ComposeAppManager::Config::Config(const PackageConfig& pconfig) {
  const std::map<std::string, std::string> raw = pconfig.extra;

  if (raw.count("compose_apps") == 1) {
    std::string val = raw.at("compose_apps");
    // if compose_apps is specified then `apps` optional configuration variable is initialized with an empty vector
    apps = boost::make_optional(std::vector<std::string>());
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(*apps, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }

  if (raw.count("reset_apps") == 1) {
    std::string val = raw.at("reset_apps");
    reset_apps = boost::make_optional(std::vector<std::string>());
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(*reset_apps, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }

  if (raw.count("compose_apps_root") == 1) {
    apps_root = raw.at("compose_apps_root");
  }
  if (raw.count("reset_apps_root") == 1) {
    reset_apps_root = raw.at("reset_apps_root");
  }
  if (raw.count("compose_apps_tree") == 1) {
    apps_tree = raw.at("compose_apps_tree");
  }
  if (raw.count("create_apps_tree") == 1) {
    create_apps_tree = boost::lexical_cast<bool>(raw.at("create_apps_tree"));
  }
  if (raw.count("images_data_root") == 1) {
    images_data_root = raw.at("images_data_root");
  }
  if (raw.count("docker_images_reload_cmd") == 1) {
    docker_images_reload_cmd = raw.at("docker_images_reload_cmd");
  }
  if (raw.count("docker_compose_bin") == 1) {
    compose_bin = raw.at("docker_compose_bin");
  }
  if (raw.count("skopeo_bin") == 1) {
    skopeo_bin = raw.at("skopeo_bin");
  }
#ifdef USE_COMPOSEAPP_ENGINE
  if (raw.count("composectl_bin") == 1) {
    composectl_bin = raw.at("composectl_bin");
  }
#endif  // USE_COMPOSEAPP_ENGINE

  if (raw.count("docker_prune") == 1) {
    std::string val = raw.at("docker_prune");
    boost::algorithm::to_lower(val);
    docker_prune = val != "0" && val != "false";
  }

  if (raw.count("force_update") > 0) {
    force_update = boost::lexical_cast<bool>(raw.at("force_update"));
  }

  if (raw.count("hub_auth_creds_endpoint") == 1) {
    hub_auth_creds_endpoint = raw.at("hub_auth_creds_endpoint");
  }

  if (raw.count("create_containers_before_reboot") > 0) {
    create_containers_before_reboot = boost::lexical_cast<bool>(raw.at("create_containers_before_reboot"));
  }

  if (raw.count("storage_watermark") > 0) {
    const std::string storage_watermark_str{raw.at("storage_watermark")};

    try {
      storage_watermark = std::stoi(storage_watermark_str);
    } catch (const std::invalid_argument&) {
      LOG_ERROR << "Invalid sota.toml:pacman:storage_watermark value, should be an integer, got "
                << storage_watermark_str;
      throw;
    } catch (const std::out_of_range&) {
      LOG_ERROR << "Invalid sota.toml:pacman:storage_watermark value, the specified value is out the integer range: "
                << storage_watermark_str;
      throw;
    } catch (const std::exception& exc) {
      LOG_ERROR
          << "Unexpected error while processing sota.toml:pacman:storage_watermark value, should be an integer got "
          << storage_watermark_str << ", err: " << exc.what();
      throw;
    }
  }
}

ComposeAppManager::ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                                     const std::shared_ptr<INvStorage>& storage,
                                     const std::shared_ptr<HttpInterface>& http,
                                     std::shared_ptr<OSTree::Sysroot> sysroot, const KeyManager& keys,
                                     AppEngine::Ptr app_engine)
    : RootfsTreeManager(pconfig, bconfig, storage, http, std::move(sysroot), keys),
      cfg_{pconfig},
      app_engine_{std::move(app_engine)} {
  if (!app_engine_) {
    auto registry_client{std::make_shared<Docker::RegistryClient>(http, cfg_.hub_auth_creds_endpoint)};
    std::string compose_cmd{boost::filesystem::canonical(cfg_.compose_bin).string() + " "};

    if (cfg_.compose_bin.filename().compare("docker") == 0) {
      // if it is a `docker` binary then turn it into ` the  `docker compose` command
      // and make sure that the `compose` is actually supported by a given `docker` utility.
      compose_cmd += "compose ";

      // To be removed once LmP/meta-lmp moves to `docker compose` by default
      std::string out;
      if (Utils::shell(compose_cmd, &out) != EXIT_SUCCESS) {
        LOG_WARNING << "The docker utility specified in the config does not support `compose` mode: " << compose_cmd;

        compose_cmd = boost::filesystem::canonical("/usr/bin/docker-compose").string() + " ";
        LOG_WARNING << "Falling back to the python docker-compose: " << compose_cmd;
      }
    }
    LOG_DEBUG << "Compose utility: `" << compose_cmd << "`";

    std::string docker_host{"unix:///var/run/docker.sock"};

    if (!!cfg_.reset_apps) {
      auto env{boost::this_process::environment()};
      if (env.end() != env.find("DOCKER_HOST")) {
        docker_host = env.get("DOCKER_HOST");
      }
#ifdef USE_COMPOSEAPP_ENGINE
      const auto composectl_cmd{boost::filesystem::canonical(cfg_.composectl_bin).string()};
      app_engine_ = std::make_shared<composeapp::AppEngine>(
          cfg_.reset_apps_root, cfg_.apps_root, cfg_.images_data_root, registry_client,
          std::make_shared<Docker::DockerClient>(), docker_host, compose_cmd, composectl_cmd, cfg_.storage_watermark,
          Docker::RestorableAppEngine::GetDefStorageSpaceFunc(cfg_.storage_watermark));
#else
      const std::string skopeo_cmd{boost::filesystem::canonical(cfg_.skopeo_bin).string()};
      app_engine_ = std::make_shared<Docker::RestorableAppEngine>(
          cfg_.reset_apps_root, cfg_.apps_root, cfg_.images_data_root, registry_client,
          std::make_shared<Docker::DockerClient>(), skopeo_cmd, docker_host, compose_cmd,
          Docker::RestorableAppEngine::GetDefStorageSpaceFunc(cfg_.storage_watermark));
#endif  // USE_COMPOSEAPP_ENGINE
      is_restorable_engine_ = true;
    } else {
      app_engine_ = std::make_shared<Docker::ComposeAppEngine>(
          cfg_.apps_root, compose_cmd, std::make_shared<Docker::DockerClient>(), registry_client);
    }
  }
}

// Returns an intersection of apps specified in Target and the configuration
ComposeAppManager::AppsContainer ComposeAppManager::getApps(const Uptane::Target& t) const {
  AppsContainer apps;

  auto target_apps = t.custom_data()["docker_compose_apps"];
  for (Json::ValueIterator i = target_apps.begin(); i != target_apps.end(); ++i) {
    if ((*i).isObject() && (*i).isMember("uri")) {
      const auto& target_app_name = i.key().asString();
      const auto& target_app_uri = (*i)["uri"].asString();

      if (!!cfg_.apps) {
        // if `compose_apps` is specified in the config then add the current Target app only if it listed in
        // `compose_apps`
        for (const auto& app : *(cfg_.apps)) {
          if (target_app_name == app) {
            apps[target_app_name] = target_app_uri;
            break;
          }
        }
      } else {
        // if `compose_apps` is not specified just add all Target's apps
        apps[target_app_name] = target_app_uri;
      }

    } else {
      LOG_ERROR << "Invalid custom data for docker_compose_app: " << i.key().asString() << " -> " << *i;
    }
  }

  return apps;
}

ComposeAppManager::AppsContainer ComposeAppManager::getAppsToUpdate(const Uptane::Target& t,
                                                                    AppsSyncReason& apps_and_reasons) const {
  AppsContainer apps_to_update;

  auto currently_installed_target_apps = Target::appsJson(OstreeManager::getCurrent());
  auto new_target_apps = getApps(t);  // intersection of apps specified in Target and the configuration

  for (const auto& app_pair : new_target_apps) {
    const auto& app_name = app_pair.first;

    auto app_data = currently_installed_target_apps.get(app_name, Json::nullValue);
    if (app_data.empty()) {
      // new app in Target
      apps_to_update.insert(app_pair);
      apps_and_reasons[app_pair.first] = "new app in target";
      LOG_INFO << app_name << " will be installed";
      continue;
    }

    if (app_pair.second != app_data["uri"].asString()) {
      // an existing App update
      apps_to_update.insert(app_pair);
      apps_and_reasons[app_pair.first] = "new version in target";
      LOG_INFO << app_name << " will be updated";
      continue;
    }

    if (!boost::filesystem::exists(cfg_.apps_root / app_name) ||
        !boost::filesystem::exists(cfg_.apps_root / app_name / Docker::ComposeAppEngine::ComposeFile)) {
      // an App that is supposed to be installed has been removed somehow, let's install it again
      apps_to_update.insert(app_pair);
      apps_and_reasons[app_pair.first] = "missing installation, to be re-installed";
      LOG_INFO << app_name << " will be re-installed";
      continue;
    }

    LOG_DEBUG << app_name << " performing full status check";
    if (!app_engine_->isFetched({app_name, app_pair.second})) {
      // an App that is supposed to be installed is not fully installed
      apps_to_update.insert(app_pair);
      apps_and_reasons[app_pair.first] = "not fetched";
      LOG_INFO << app_name << " is not fully fetched; missing blobs will be fetched";
      continue;
    }
    if (!app_engine_->isRunning({app_name, app_pair.second})) {
      // an App that is supposed to running is not running
      apps_to_update.insert(app_pair);
      apps_and_reasons[app_pair.first] = "not running";
      LOG_INFO << app_name << " is not installed or not running; will be installed and started";
      continue;
    }
  }

  return apps_to_update;
}

ComposeAppManager::AppsSyncReason ComposeAppManager::checkForAppsToUpdate(const Uptane::Target& target) {
  AppsSyncReason apps_and_reasons;
  cur_apps_to_fetch_and_update_ = getAppsToUpdate(target, apps_and_reasons);
  if (!!cfg_.reset_apps) {
    cur_apps_to_fetch_ = getAppsToFetch(target);
  }
  are_apps_checked_ = true;
  for (const auto& app : cur_apps_to_fetch_) {
    if (apps_and_reasons.count(app.first) == 0) {
      apps_and_reasons[app.first] = "not fetched (reset apps)";
    }
  }
  return apps_and_reasons;
}

DownloadResult ComposeAppManager::Download(const TufTarget& target) {
  auto ostree_download_res{RootfsTreeManager::Download(target)};
  if (!ostree_download_res) {
    return ostree_download_res;
  }

  DownloadResult res{ostree_download_res};
  const Uptane::Target uptane_target{Target::fromTufTarget(target)};

  if (cfg_.force_update) {
    LOG_INFO << "All Apps are forced to be updated...";
    cur_apps_to_fetch_and_update_ = getApps(uptane_target);
  } else if (!are_apps_checked_) {
    // non-daemon mode (force check) or a new Target to be applied in daemon mode,
    // then do full check if Target Apps are installed and running
    LOG_INFO << "Checking for Apps to be installed or updated...";
    checkForAppsToUpdate(uptane_target);
  }

  LOG_INFO << "Found " << cur_apps_to_fetch_and_update_.size() << " Apps to update";

  AppsContainer all_apps_to_fetch;
  all_apps_to_fetch.insert(cur_apps_to_fetch_and_update_.begin(), cur_apps_to_fetch_and_update_.end());
  all_apps_to_fetch.insert(cur_apps_to_fetch_.begin(), cur_apps_to_fetch_.end());

  std::stringstream stat_msg;
  if (!all_apps_to_fetch.empty()) {
    const auto pre_pull_fs_usage{getAppsFsUsageInfo()};
    stat_msg << res.description << "\nbefore apps pull: " << pre_pull_fs_usage;
    LOG_INFO << "Pre Apps pull storage usage info; " << pre_pull_fs_usage;
  }
  for (const auto& pair : all_apps_to_fetch) {
    LOG_INFO << "Fetching " << pair.first << " -> " << pair.second;
    const auto fetch_res{app_engine_->fetch({pair.first, pair.second})};
    if (!fetch_res) {
      const std::string err_desc{boost::str(boost::format("failed to fetch App; app: %s; uri: %s; %s") % pair.first %
                                            pair.second % fetch_res.err)};
      LOG_ERROR << err_desc;
      stat_msg << "\n" << err_desc;
      if (fetch_res.noSpace()) {
        res = {DownloadResult::Status::DownloadFailed_NoSpace, stat_msg.str(), fetch_res.stat.path, fetch_res.stat};
      } else {
        res = {DownloadResult::Status::DownloadFailed, ""};
      }
      break;
    }
  }

  if (!all_apps_to_fetch.empty() && !res.noSpace()) {
    const auto post_pull_fs_usage{getAppsFsUsageInfo()};
    stat_msg << "\nafter apps pull: " << post_pull_fs_usage;
    res.description = stat_msg.str();
    LOG_INFO << "Post Apps pull storage usage info; " << post_pull_fs_usage;
  }

  are_apps_checked_ = false;
  return res;
}

bool ComposeAppManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  (void)target;
  (void)fetcher;
  (void)token;
  (void)progress_cb;
  (void)keys;

  throw std::runtime_error("Using obsolete method of package manager: fetchTarget()");
}

TargetStatus ComposeAppManager::verifyTarget(const Uptane::Target& target) const {
  const auto ostree_target_status{RootfsTreeManager::verifyTarget(target)};
  if (TargetStatus::kGood != ostree_target_status) {
    return ostree_target_status;
  }

  AppsContainer all_apps_to_fetch;
  all_apps_to_fetch.insert(cur_apps_to_fetch_and_update_.begin(), cur_apps_to_fetch_and_update_.end());
  all_apps_to_fetch.insert(cur_apps_to_fetch_.begin(), cur_apps_to_fetch_.end());

  for (const auto& pair : all_apps_to_fetch) {
    if (!app_engine_->isFetched({pair.first, pair.second})) {
      return TargetStatus::kNotFound;
    }

    if (!app_engine_->verify({pair.first, pair.second})) {
      return TargetStatus::kInvalid;
    }
  }
  return ostree_target_status;
}

data::InstallationResult ComposeAppManager::Install(const TufTarget& target, InstallMode mode) {
  if (mode == InstallMode::OstreeOnly) {
    return RootfsTreeManager::Install(target, mode);
  }
  return install(Target::fromTufTarget(target));
}

data::InstallationResult ComposeAppManager::install(const Uptane::Target& target) const {
  // Stopping disabled apps before creating or starting new apps
  // because they may interfere with each other (e.g., using the same port).
  // It is advisable to stop the disabled apps before performing ostree installation.
  // Otherwise, they will be automatically started by dockerd during boot if the device
  // is shut down immediately after a successful ostree installation and before the disabled apps are stopped.
  // The probability of ostree installation failure is very low. Even if it occurs,
  // the subsequent "sync target" process will restart the apps (excluding those removed from the configuration).
  stopDisabledComposeApps(target);

  if (getCurrent().sha256Hash() != target.sha256Hash()) {
    // If this ostree + apps update, then stop the Apps that is about to be updated
    // so they are not started automatically by dockerd just after reboot.
    // If ostree is not updated then the updated Apps will be started in this context.
    // If ostree is updated and a device is suddenly rebooted before Apps installation, then
    // it ensures that the previous version Apps are not automatically started on boot.
    // If an installation failure happens, then the following "sync target" process will re-start
    // the stopped Apps (app only rollback).
    for (const auto& pair : cur_apps_to_fetch_and_update_) {
      LOG_INFO << "Stopping App before updating it; " << pair.first << " -> " << pair.second;
      auto& non_const_app_engine = (const_cast<ComposeAppManager*>(this))->app_engine_;
      non_const_app_engine->stop({pair.first, pair.second});
    }
  }

  data::InstallationResult res{RootfsTreeManager::install(target)};
  if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
    LOG_ERROR << "OSTree target installation has failed, skipping Docker Compose Apps";
    res.description += "\n# Apps running:\n" + getRunningAppsInfoForReport();
    return res;
  }

  bool prune_images{cfg_.docker_prune};
  const bool just_install = res.result_code == data::ResultCode::Numeric::kNeedCompletion;  // system update

  if (!just_install || cfg_.create_containers_before_reboot) {
    // make sure we install what we fetched
    if (!cur_apps_to_fetch_and_update_.empty()) {
      res.description += "\n# Apps installed:";
    }

    for (const auto& pair : cur_apps_to_fetch_and_update_) {
      LOG_INFO << "Installing " << pair.first << " -> " << pair.second;
      // I have no idea via the package manager interface method install() is const which is not a const
      // method by its definition/nature
      auto& non_const_app_engine = (const_cast<ComposeAppManager*>(this))->app_engine_;
      const AppEngine::Result run_res = just_install ? non_const_app_engine->install({pair.first, pair.second})
                                                     : non_const_app_engine->run({pair.first, pair.second});

      if (!run_res) {
        const std::string err_desc{boost::str(boost::format("failed to install App; app: %s; uri: %s; err: %s") %
                                              pair.first % pair.second % run_res.err)};
        LOG_ERROR << err_desc;

        res = data::InstallationResult(run_res.imagePullFailure() ? data::ResultCode::Numeric::kDownloadFailed
                                                                  : data::ResultCode::Numeric::kInstallFailed,
                                       err_desc);
      } else {
        res.description += "\n" + pair.second;
      }
    }

  } else {
    LOG_INFO << "Apps' containers will be re-created and started just after successful boot on the new ostree version";
    res.description += "\n# Fetched Apps' containers will be created and started after reboot\n";
    // don't prune Compose Apps' images because new images are not used by any containers and can be removed as a
    // result of pruning.
    prune_images = false;
  }

  if (res.isSuccess()) {
    // if App successfully started then clean uninstalled/disabled Apps,
    // otherwise do it just after successful finalization
    handleRemovedApps(target);

    if (prune_images) {
      AppEngine::Apps app_shortlist;
      const auto enabled_apps{getAppsToFetch(target, false)};

      std::for_each(enabled_apps.cbegin(), enabled_apps.cend(),
                    [&app_shortlist](const std::pair<std::string, std::string>& val) {
                      app_shortlist.emplace_back(AppEngine::App{val.first, val.second});
                    });

      app_engine_->prune(app_shortlist);
    }
  }

  // there is no much reason in re-trying to install app if its installation has failed for the first time
  // TODO: we might add more advanced logic here, e.g. try to install a few times and then fail
  cur_apps_to_fetch_and_update_.clear();
  cur_apps_to_fetch_.clear();

  res.description += "\n# Apps running:\n" + getRunningAppsInfoForReport();
  return res;
}

data::InstallationResult ComposeAppManager::finalizeInstall(const Uptane::Target& target) {
  auto ir = OstreeManager::finalizeInstall(target);

  if (ir.result_code.num_code == data::ResultCode::Numeric::kOk) {
    // Stop disabled Apps before creating or starting new Apps since they may interfere with each other (e.g. the same
    // port is used).
    stopDisabledComposeApps(target);
    if (ir.description != "Already booted on the required version") {
      LOG_INFO << "Starting Apps after successful boot on a new version of OSTree-based sysroot...";
    } else {
      LOG_INFO << "Installing and starting Apps...";
    }
    // "finalize" (run) Apps that were pulled and created before reboot
    for (const auto& app_pair : getApps(target)) {
      const AppEngine::Result run_res = app_engine_->run({app_pair.first, app_pair.second});
      if (!run_res) {
        const std::string err_desc{boost::str(
            boost::format("failed to start App after booting on a new sysroot version; app: %s; uri: %s; err: %s") %
            app_pair.first % app_pair.second % run_res.err)};

        LOG_ERROR << err_desc;
        // Do we need to set some flag for the uboot and trigger a system reboot in order to boot on a previous
        // ostree version, hence a proper/full rollback happens???
        ir.description += ", however " + err_desc;
        ir.description += "\n# Apps running:\n" + getRunningAppsInfoForReport();
        // this is a hack to distinguish between ostree install (rollback) and App start failures.
        // data::ResultCode::Numeric::kInstallFailed - boot on a new ostree version failed (rollback at boot)
        // data::ResultCode::Numeric::kCustomError - boot on a new version was successful but new App failed to start
        return data::InstallationResult(run_res.imagePullFailure() ? data::ResultCode::Numeric::kDownloadFailed
                                                                   : data::ResultCode::Numeric::kCustomError,
                                        ir.description);
      }
    }
    handleRemovedApps(target);
    if (cfg_.docker_prune) {
      AppEngine::Apps app_shortlist;
      const auto enabled_apps{getAppsToFetch(target, false)};

      std::for_each(enabled_apps.cbegin(), enabled_apps.cend(),
                    [&app_shortlist](const std::pair<std::string, std::string>& val) {
                      app_shortlist.emplace_back(AppEngine::App{val.first, val.second});
                    });

      app_engine_->prune(app_shortlist);
    }
  }

  if (data::ResultCode::Numeric::kNeedCompletion != ir.result_code.num_code) {
    ir.description += "\n# Apps running:\n" + getRunningAppsInfoForReport();
  }
  return ir;
}

void ComposeAppManager::handleRemovedApps(const Uptane::Target& target) const {
  removeDisabledComposeApps(target);

  // prune the restorable app store, make sure we remove unused blobs of Apps removed in the previous call,
  // as well as the apps and their blobs that had been only in `reset_apps` list and were removed from it
  if (cfg_.docker_prune && !!cfg_.reset_apps) {
    // get all Apps that we need to keep in the store
    const auto required_apps{getAppsToFetch(target, false)};
    // remove not required apps, only apps listed in required_apps will be preserved
    AppEngine::Apps app_shortlist;
    std::for_each(required_apps.cbegin(), required_apps.cend(),
                  [&app_shortlist](const std::pair<std::string, std::string>& val) {
                    app_shortlist.emplace_back(AppEngine::App{val.first, val.second});
                  });
    app_engine_->prune(app_shortlist);
  }
}

void ComposeAppManager::stopDisabledComposeApps(const Uptane::Target& target) const {
  forEachRemovedApp(target, [](AppEngine::Ptr& app_engine, const std::string& app_name) {
    LOG_WARNING << "Docker Compose App(" << app_name
                << ") installed, "
                   "but is either removed from configuration or not defined in current Target. "
                   "Stopping "
                << app_name;
    app_engine->stop({app_name, ""});
  });
}

// Handle the case like:
//  1) sota.toml is configured with 2 compose apps: "app1, app2"
//  2) update is applied, so we are now running both app1 and app2
//  3) sota.toml is updated with 1 docker app: "app1"
// At this point we should stop app2 and remove it.
void ComposeAppManager::removeDisabledComposeApps(const Uptane::Target& target) const {
  forEachRemovedApp(target, [](AppEngine::Ptr& app_engine, const std::string& app_name) {
    LOG_WARNING << "Docker Compose App(" << app_name
                << ") installed, "
                   "but is either removed from configuration or not defined in current Target. "
                   "Removing from system";
    app_engine->remove({app_name, ""});
  });
}

void ComposeAppManager::forEachRemovedApp(
    const Uptane::Target& target,
    const std::function<void(AppEngine::Ptr& app_engine, const std::string& app_name)>& action) const {
  if (!boost::filesystem::is_directory(cfg_.apps_root)) {
    LOG_DEBUG << "cfg_.apps_root does not exist";
    return;
  }

  // an intersection of apps specified in Target and the configuration
  // i.e. the apps that are supposed to be installed and running
  const auto& current_apps = getApps(target);

  for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(cfg_.apps_root), {})) {
    if (boost::filesystem::is_directory(entry)) {
      std::string name = entry.path().filename().native();
      if (current_apps.find(name) == current_apps.end()) {
        auto& non_const_app_engine = (const_cast<ComposeAppManager*>(this))->app_engine_;
        action(non_const_app_engine, name);
      }
    }
  }
}

void ComposeAppManager::completeInitialTarget(Uptane::Target& init_target) {
  const AppEngine::Apps installed_apps{app_engine_->getInstalledApps()};
  Json::Value apps_json;
  for (const auto& app : installed_apps) {
    apps_json[app.name]["uri"] = app.uri;
  }
  auto custom{init_target.custom_data()};
  custom[Target::ComposeAppField] = apps_json;
  init_target.updateCustom(custom);
}

Json::Value ComposeAppManager::getRunningAppsInfo() const { return app_engine_->getRunningAppsInfo(); }
std::string ComposeAppManager::getRunningAppsInfoForReport() const {
  std::string result;
  const auto apps_info = getRunningAppsInfo();
  for (Json::ValueConstIterator ii = apps_info.begin(); ii != apps_info.end(); ++ii) {
    result += ii.key().asString() + ": " + (*ii)["uri"].asString() + "; state: " + (*ii)["state"].asString() + "\n";
    ;

    Json::Value services = (*ii)["services"];
    for (Json::ValueConstIterator jj = services.begin(); jj != services.end(); ++jj) {
      result += "\t" + (*jj)["name"].asString() + ": " + (*jj)["hash"].asString();
      result += "; image: " + (*jj)["image"].asString();
      result += "; state: " + (*jj)["state"].asString();
      result += "; status: " + (*jj)["status"].asString() + "\n";
    }
  }
  return result;
}

Json::Value ComposeAppManager::getAppsState() const {
  Json::Value apps_state;

  try {
    auto apps{app_engine_->getRunningAppsInfo()};

    if (!apps.isNull()) {
      for (Json::ValueIterator ii = apps.begin(); ii != apps.end(); ++ii) {
        const auto app_name{ii.key().asString()};
        const Json::Value& services{(*ii)["services"]};

        (*ii)["state"] = "healthy";
        for (Json::ValueConstIterator jj = services.begin(); jj != services.end(); ++jj) {
          const Json::Value service{(*jj)};
          // determine a service health based on its `health` field value if present
          if (service["health"] == "unhealthy") {
            // if it's unhealthy then an overall App is considered unhealthy
            (*ii)["state"] = "unhealthy";
            break;
          }
        }

        if (!(*ii).isMember("uri")) {
          // App's URI is stored in the `state` in the case of a regular Compose App engine
          (*ii)["uri"] = "";  // TODO: figure out an app's URI
        }
      }  // for each App
    }

    const auto hash{sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent)};
    apps_state["deviceTime"] = TimeStamp::Now().ToString();
    apps_state["ostree"] = hash;
    apps_state["apps"] = apps;

  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to obtain information about running Apps: " << exc.what();
  }

  return apps_state;
}

bool ComposeAppManager::compareAppsStates(const Json::Value& left, const Json::Value& right) {
  // Unfortunately we cannot just compare json docs (Json::Value == Json::Value) because the App status
  // includes Apps running duration which obviously changes as time goes.
  // An input jsons are dicts of Apps, an app name is the dict key.
  if (!left.isMember("apps") && !right.isMember("apps")) {
    // no states at all, considered equal
    return true;
  }
  if (left.size() != right.size()) {
    // "apps" are present in one and missing in another
    return false;
  }
  if (left["apps"].size() != right["apps"].size()) {
    // Different quantity of Apps
    return false;
  }
  for (Json::ValueConstIterator ii = left["apps"].begin(); ii != left["apps"].end(); ++ii) {
    const auto app_name{ii.key().asString()};
    if (!right["apps"].isMember(app_name)) {
      // Different set of Apps
      return false;
    }
    if ((*ii)["state"] != right["apps"][app_name]["state"] || (*ii)["uri"] != right["apps"][app_name]["uri"]) {
      return false;
    }
    if ((*ii)["services"].size() != right["apps"][app_name]["services"].size()) {
      return false;
    }
  }
  return true;
}

ComposeAppManager::AppsContainer ComposeAppManager::getRequiredApps(const Config& cfg, const Uptane::Target& target) {
  AppsContainer apps;
  std::set<std::string> cfg_apps_union;
  const auto& target_apps = Target::Apps(target);

  if (!!cfg.apps) {
    cfg_apps_union.insert((*cfg.apps).begin(), (*cfg.apps).end());
  } else {
    std::for_each(target_apps.begin(), target_apps.end(),
                  [&cfg_apps_union](const Target::Apps::AppDesc& val) { cfg_apps_union.insert(val.name); });
  }

  if (!!cfg.reset_apps) {
    const auto& reset_apps{*cfg.reset_apps};
    if (reset_apps.size() == 1 && reset_apps[0] == "*") {
      // if `reset_apps="*"`, then all target apps should be fetched and stored
      std::for_each(target_apps.begin(), target_apps.end(),
                    [&cfg_apps_union](const Target::Apps::AppDesc& val) { cfg_apps_union.insert(val.name); });
    } else {
      cfg_apps_union.insert(reset_apps.begin(), reset_apps.end());
    }
  }

  for (const auto& app_name : cfg_apps_union) {
    if (!target_apps.isPresent(app_name)) {
      continue;
    }

    const auto app{target_apps[app_name]};
    apps[app.name] = app.uri;
  }

  return apps;
}

ComposeAppManager::AppsContainer ComposeAppManager::getAppsToFetch(const Uptane::Target& target,
                                                                   bool check_store) const {
  auto enabled_apps{getRequiredApps(cfg_, target)};
  if (!check_store) {
    return enabled_apps;
  }

  AppsContainer apps_to_be_fetched;
  for (const auto& app : enabled_apps) {
    if (!app_engine_->isFetched({app.first, app.second})) {
      apps_to_be_fetched.insert(app);
    }
  }

  return apps_to_be_fetched;
}

std::string ComposeAppManager::getAppsFsUsageInfo() const {
  std::stringstream ss;
  auto usage_info{storage::Volume::getUsageInfo(cfg_.images_data_root.string(), (100 - cfg_.storage_watermark),
                                                "pacman:storage_watermark")};
  if (!usage_info.isOk()) {
    LOG_ERROR << "Failed to obtain storage usage statistic: " << usage_info.err;
  }
  ss << usage_info;
  if (is_restorable_engine_ &&
      !Docker::RestorableAppEngine::areDockerAndSkopeoOnTheSameVolume(cfg_.apps_root, cfg_.images_data_root)) {
    auto usage_info{storage::Volume::getUsageInfo(cfg_.apps_root.string(), (100 - cfg_.storage_watermark),
                                                  "pacman:storage_watermark")};
    if (!usage_info.isOk()) {
      LOG_ERROR << "Failed to obtain storage usage statistic: " << usage_info.err;
    }
    ss << "\n" << usage_info;
  }
  return ss.str();
}
