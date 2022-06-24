#include "composeappmanager.h"

#include <set>

#include "bootloader/bootloaderlite.h"
#include "docker/restorableappengine.h"
#include "target.h"

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
}

ComposeAppManager::ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                                     const std::shared_ptr<INvStorage>& storage,
                                     const std::shared_ptr<HttpInterface>& http,
                                     std::shared_ptr<OSTree::Sysroot> sysroot, AppEngine::Ptr app_engine)
    : RootfsTreeManager(pconfig, bconfig, storage, http, std::move(sysroot)),
      cfg_{pconfig},
      app_engine_{std::move(app_engine)} {
  if (!app_engine_) {
    auto docker_client{std::make_shared<Docker::DockerClient>()};
    auto registry_client{std::make_shared<Docker::RegistryClient>(http, cfg_.hub_auth_creds_endpoint)};
    const auto compose_cmd{boost::filesystem::canonical(cfg_.compose_bin).string() + " "};
    const std::string skopeo_cmd{boost::filesystem::canonical(cfg_.skopeo_bin).string()};
    const std::string docker_host{"unix:///var/run/docker.sock"};

    if (!!cfg_.reset_apps) {
      app_engine_ = std::make_shared<Docker::RestorableAppEngine>(cfg_.reset_apps_root, cfg_.apps_root,
                                                                  cfg_.images_data_root, registry_client, docker_client,
                                                                  skopeo_cmd, docker_host, compose_cmd);
    } else {
      app_engine_ =
          std::make_shared<Docker::ComposeAppEngine>(cfg_.apps_root, compose_cmd, docker_client, registry_client);
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

ComposeAppManager::AppsContainer ComposeAppManager::getAppsToUpdate(const Uptane::Target& t) const {
  AppsContainer apps_to_update;

  auto currently_installed_target_apps = Target::appsJson(OstreeManager::getCurrent());
  auto new_target_apps = getApps(t);  // intersection of apps specified in Target and the configuration

  for (const auto& app_pair : new_target_apps) {
    const auto& app_name = app_pair.first;

    auto app_data = currently_installed_target_apps.get(app_name, Json::nullValue);
    if (app_data.empty()) {
      // new app in Target
      apps_to_update.insert(app_pair);
      LOG_INFO << app_name << " will be installed";
      continue;
    }

    if (app_pair.second != app_data["uri"].asString()) {
      // an existing App update
      apps_to_update.insert(app_pair);
      LOG_INFO << app_name << " will be updated";
      continue;
    }

    if (!boost::filesystem::exists(cfg_.apps_root / app_name) ||
        !boost::filesystem::exists(cfg_.apps_root / app_name / Docker::ComposeAppEngine::ComposeFile)) {
      // an App that is supposed to be installed has been removed somehow, let's install it again
      apps_to_update.insert(app_pair);
      LOG_INFO << app_name << " will be re-installed";
      continue;
    }

    LOG_DEBUG << app_name << " performing full status check";
    if (!app_engine_->isRunning({app_name, app_pair.second})) {
      // an App that is supposed to be installed and running is not fully installed or running
      apps_to_update.insert(app_pair);
      LOG_INFO << app_name << " update will be re-installed or completed";
      continue;
    }
  }

  return apps_to_update;
}

bool ComposeAppManager::checkForAppsToUpdate(const Uptane::Target& target) {
  cur_apps_to_fetch_and_update_ = getAppsToUpdate(target);
  if (!!cfg_.reset_apps) {
    cur_apps_to_fetch_ = getAppsToFetch(target);
  }
  are_apps_checked_ = true;
  return cur_apps_to_fetch_and_update_.empty() && cur_apps_to_fetch_.empty();
}

bool ComposeAppManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  if (!RootfsTreeManager::fetchTarget(target, fetcher, keys, progress_cb, token)) {
    return false;
  }

  if (cfg_.force_update) {
    LOG_INFO << "All Apps are forced to be updated...";
    cur_apps_to_fetch_and_update_ = getApps(target);
  } else if (!are_apps_checked_) {
    // non-daemon mode (force check) or a new Target to be applied in daemon mode,
    // then do full check if Target Apps are installed and running
    LOG_INFO << "Checking for Apps to be installed or updated...";
    checkForAppsToUpdate(target);
  }

  LOG_INFO << "Found " << cur_apps_to_fetch_and_update_.size() << " Apps to update";

  bool passed = true;
  AppsContainer all_apps_to_fetch;
  all_apps_to_fetch.insert(cur_apps_to_fetch_and_update_.begin(), cur_apps_to_fetch_and_update_.end());
  all_apps_to_fetch.insert(cur_apps_to_fetch_.begin(), cur_apps_to_fetch_.end());

  for (const auto& pair : all_apps_to_fetch) {
    LOG_INFO << "Fetching " << pair.first << " -> " << pair.second;
    if (!app_engine_->fetch({pair.first, pair.second})) {
      passed = false;
    }
  }

  are_apps_checked_ = false;
  return passed;
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
    if (!app_engine_->verify({pair.first, pair.second})) {
      return TargetStatus::kInvalid;
    }
  }
  return ostree_target_status;
}

data::InstallationResult ComposeAppManager::install(const Uptane::Target& target) const {
  data::InstallationResult res;
  Uptane::Target current = OstreeManager::getCurrent();
  // Do ostree install if the currently installed target's hash differs from the specified target's hash,
  // or there is pending installation and it differs from the specified target so we undeploy it and make the new target
  // pending
  if ((current.sha256Hash() != target.sha256Hash()) ||
      (!sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty() &&
       sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kPending) != target.sha256Hash())) {
    // notify the bootloader before installation happens as it is not atomic
    // and a false notification doesn't hurt with rollback support in place
    // Hacking in order to invoke non-const method from the const one !!!
    const_cast<ComposeAppManager*>(this)->updateNotify();
    res = OstreeManager::install(target);
    if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
      LOG_ERROR << "Failed to install OSTree target, skipping Docker Compose Apps";
      res.description += "\n# Apps running:\n" + getRunningAppsInfoForReport();
      return res;
    }
    const_cast<ComposeAppManager*>(this)->installNotify(target);
    if (current.sha256Hash() == target.sha256Hash() &&
        res.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
      LOG_INFO << "Successfully undeployed the pending failing Target";
      LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
      res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
    }
  } else {
    LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
    res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
  }

  bool prune_images{cfg_.docker_prune};
  const bool just_install = res.result_code == data::ResultCode::Numeric::kNeedCompletion;  // system update

  if (!just_install || cfg_.create_containers_before_reboot) {
    // Stop disabled Apps before creating or starting new Apps since they may interfere with each other (e.g. the same
    // port is used).
    stopDisabledComposeApps(target);
    // make sure we install what we fecthed
    if (!cur_apps_to_fetch_and_update_.empty()) {
      res.description += "\n# Apps installed:";
    }

    for (const auto& pair : cur_apps_to_fetch_and_update_) {
      LOG_INFO << "Installing " << pair.first << " -> " << pair.second;
      // I have no idea via the package manager interface method install() is const which is not a const
      // method by its definition/nature
      auto& non_const_app_engine = (const_cast<ComposeAppManager*>(this))->app_engine_;
      bool run_res = just_install ? non_const_app_engine->install({pair.first, pair.second})
                                  : non_const_app_engine->run({pair.first, pair.second});

      if (!run_res) {
        res = data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Could not install app");
      } else {
        res.description += "\n" + pair.second;
      }
    }

  } else {
    LOG_INFO << "Apps' containers will be re-created and started just after succesfull boot on the new ostree version";
    res.description += "\n# Fecthed Apps' containers will be created and started after reboot\n";
    // don't prune Compose Apps' images because new images are not used by any containers and can be removed as a
    // result of prunning.
    prune_images = false;
  }

  if (res.isSuccess()) {
    // if App successfully started then clean uninstalled/disabled Apps,
    // otherwise do it just after successfull finalization
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
    LOG_INFO << "Starting Apps after successful boot on a new version of OSTree-based sysroot...";
    // "finalize" (run) Apps that were pulled and created before reboot
    for (const auto& app_pair : getApps(target)) {
      if (!app_engine_->run({app_pair.first, app_pair.second})) {
        LOG_ERROR << "Failed to start App after booting on a new version of OSTree-based sysroot;"
                     " app: "
                  << app_pair.first << "; uri: " << app_pair.second << "; " << target.filename();
        // Do we need to set some flag for the uboot and trigger a system reboot in order to boot on a previous
        // ostree version, hence a proper/full rollback happens???
        ir.description += "\n# Failed to start App after booting on a new sysroot version: " + app_pair.first +
                          "; uri: " + app_pair.second;
        ir.description += "\n# Apps running:\n" + getRunningAppsInfoForReport();
        // this is a hack to distinguish between ostree install (rollback) and App start failures.
        // data::ResultCode::Numeric::kInstallFailed - boot on a new ostree version failed (rollback at boot)
        // data::ResultCode::Numeric::kCustomError - boot on a new version was successful but new App failed to start
        return data::InstallationResult(data::ResultCode::Numeric::kCustomError, ir.description);
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
  if (!!cfg_.reset_apps) {
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
        LOG_WARNING << "Docker Compose App(" << name
                    << ") installed, "
                       "but is either removed from configuration or not defined in current Target. "
                       "Stopping it";

        // I have no idea via the package manager interface method install() is const which is not a const
        // method by its definition/nature
        auto& non_const_app_engine = (const_cast<ComposeAppManager*>(this))->app_engine_;
        non_const_app_engine->stop({name, ""});
      }
    }
  }
}

// Handle the case like:
//  1) sota.toml is configured with 2 compose apps: "app1, app2"
//  2) update is applied, so we are now running both app1 and app2
//  3) sota.toml is updated with 1 docker app: "app1"
// At this point we should stop app2 and remove it.
void ComposeAppManager::removeDisabledComposeApps(const Uptane::Target& target) const {
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
        LOG_WARNING << "Docker Compose App(" << name
                    << ") installed, "
                       "but is either removed from configuration or not defined in current Target. "
                       "Removing from system";

        // I have no idea via the package manager interface method install() is const which is not a const
        // method by its definition/nature
        auto& non_const_app_engine = (const_cast<ComposeAppManager*>(this))->app_engine_;
        non_const_app_engine->remove({name, ""});
      }
    }
  }
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
      result += "\t" + jj.key().asString() + ": " + (*jj)["hash"].asString();
      result += "; image: " + (*jj)["image"].asString();
      result += "; state: " + (*jj)["state"].asString();
      result += "; status: " + (*jj)["status"].asString() + "\n";
    }
  }
  return result;
}

ComposeAppManager::AppsContainer ComposeAppManager::getAppsToFetch(const Uptane::Target& target,
                                                                   bool check_store) const {
  AppsContainer apps;
  std::set<std::string> cfg_apps_union;
  const auto& target_apps = Target::Apps(target);

  if (!!cfg_.apps) {
    cfg_apps_union.insert((*cfg_.apps).begin(), (*cfg_.apps).end());
  } else {
    std::for_each(target_apps.begin(), target_apps.end(),
                  [&cfg_apps_union](const Target::Apps::AppDesc& val) { cfg_apps_union.insert(val.name); });
  }

  if (!!cfg_.reset_apps) {
    cfg_apps_union.insert((*cfg_.reset_apps).begin(), (*cfg_.reset_apps).end());
  }

  for (const auto& app_name : cfg_apps_union) {
    if (!target_apps.isPresent(app_name)) {
      continue;
    }

    const auto app{target_apps[app_name]};
    if (!check_store || !app_engine_->isFetched({app.name, app.uri})) {
      apps[app.name] = app.uri;
    }
  }

  return apps;
}
