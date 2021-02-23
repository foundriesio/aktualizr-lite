#include "composeappmanager.h"

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

  if (raw.count("compose_apps_root") == 1) {
    apps_root = raw.at("compose_apps_root");
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

  if (raw.count("docker_bin") == 1) {
    docker_bin = raw.at("docker_bin");
  }

  if (raw.count("docker_prune") == 1) {
    std::string val = raw.at("docker_prune");
    boost::algorithm::to_lower(val);
    docker_prune = val != "0" && val != "false";
  }

  if (raw.count("force_update") > 0) {
    force_update = boost::lexical_cast<bool>(raw.at("force_update"));
  }
}

ComposeAppManager::ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                                     const std::shared_ptr<INvStorage>& storage,
                                     const std::shared_ptr<HttpInterface>& http,
                                     std::shared_ptr<OSTree::Sysroot> sysroot,
                                     Docker::RegistryClient::HttpClientFactory registry_http_client_factory)
    : OstreeManager(pconfig, bconfig, storage, http),
      cfg_{pconfig},
      sysroot_{std::move(sysroot)},
      registry_client_{pconfig.ostree_server, http, std::move(registry_http_client_factory)},
      app_ctor_{[this](const std::string& app) {
        return Docker::ComposeApp(app, cfg_.apps_root, boost::filesystem::canonical(cfg_.compose_bin).string() + " ",
                                  boost::filesystem::canonical(cfg_.docker_bin).string() + " ", registry_client_);
      }} {
  try {
    app_tree_ = std::make_unique<ComposeAppTree>(cfg_.apps_tree.string(), cfg_.apps_root.string(),
                                                 cfg_.images_data_root.string(), cfg_.create_apps_tree);
  } catch (const std::exception& exc) {
    LOG_DEBUG << "Failed to initialize Compose App Tree (ostree) at " << cfg_.apps_tree << ". Error: " << exc.what();
  }

  Target::Apps current_apps;
  try {
    // Get all Target Apps
    Uptane::Target current_target = OstreeManager::getCurrent();
    if (current_target.IsValid()) {
      current_apps = Target::targetApps(current_target, boost::none);
    }
  } catch (const std::exception& exp) {
    LOG_WARNING << "Failed to get Target Apps: " << exp.what();
  }

  for (const auto& app_pair : current_apps) {
    const auto& app_name = app_pair.first;
    auto need_start_flag = cfg_.apps_root / app_name / Docker::ComposeApp::NeedStartFile;
    if (boost::filesystem::exists(need_start_flag)) {
      app_ctor_(app_name).start();
      boost::filesystem::remove(need_start_flag);
    }
  }
}

Uptane::Target ComposeAppManager::getCurrent() const {
  Uptane::Target current_from_ostree_manager = OstreeManager::getCurrent();
  if (!current_from_ostree_manager.IsValid()) {
    return current_from_ostree_manager;
  }

  // shortlisted Target apps
  const auto target_apps = Target::targetApps(current_from_ostree_manager, cfg_.apps);
  std::vector<std::string> installed_and_running_apps;

  for (const auto& app : target_apps) {
    //    auto compose_app = Docker::ComposeApp(app.first, app.second,
    //                                          cfg_.apps_root, boost::filesystem::canonical(cfg_.compose_bin).string()
    //                                          + " ", boost::filesystem::canonical(cfg_.docker_bin).string() + " ",
    //                                          registry_client_);

    //    if (compose_app.isInstalled(app.second) and compose_app.isRunning(app.second)) {
    //      installed_and_running_apps.push_back(app.first);
    //    }
    if (app_ctor_(app.first).isRunning()) {
      installed_and_running_apps.push_back(app.first);
    }
  }

  Target::shortlistTargetApps(current_from_ostree_manager, installed_and_running_apps);

  return current_from_ostree_manager;
}

bool ComposeAppManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  if (!OstreeManager::fetchTarget(target, fetcher, keys, progress_cb, token)) {
    return false;
  }

  bool passed = true;
  const auto& apps_uri = target.custom_data()["compose-apps-uri"].asString();
  if (app_tree_ && !apps_uri.empty()) {
    LOG_INFO << "Fetching Apps Tree -> " << apps_uri;

    try {
      app_tree_->pull(config.ostree_server, keys, apps_uri);
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to pull Apps Tree; uri: " << apps_uri << ", err: " << exc.what();
      passed = false;
    }

  } else {
    const auto target_apps = Target::targetApps(target, cfg_.apps);
    for (const auto& pair : target_apps) {
      LOG_INFO << "Fetching " << pair.first << " -> " << pair.second;
      if (!app_ctor_(pair.first).fetch(pair.second)) {
        passed = false;
      }
    }
  }
  are_apps_checked_ = false;
  return passed;
}

data::InstallationResult ComposeAppManager::install(const Uptane::Target& target) const {
  data::InstallationResult res;
  Uptane::Target current = OstreeManager::getCurrent();
  if (current.sha256Hash() != target.sha256Hash()) {
    // notify the bootloader before installation happens as it is not atomic
    // and a false notification doesn't hurt with rollback support in place
    // Hacking in order to invoke non-const method from the const one !!!
    const_cast<ComposeAppManager*>(this)->updateNotify();
    res = OstreeManager::install(target);
    if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
      LOG_ERROR << "Failed to install OSTree target, skipping Docker Compose Apps";
      return res;
    }
  } else {
    LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
    res = data::InstallationResult(data::ResultCode::Numeric::kAlreadyProcessed,
                                   "OSTree hash already installed, same as current");
  }

  const auto& apps_uri = target.custom_data()["compose-apps-uri"].asString();
  if (app_tree_ && !apps_uri.empty()) {
    LOG_INFO << "Checking out updated Apps: " << apps_uri;
    try {
      const_cast<ComposeAppManager*>(this)->app_tree_->checkout(apps_uri);
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to checkout Apps from the ostree repo; uri: " << apps_uri << ", err: " << exc.what();
      return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed,
                                      "Could not checkout Apps from the ostree repo");
    }

    LOG_INFO << "Reloading the docker image and layer store to enable the update... ";
    {
      const auto& cmd = cfg_.docker_images_reload_cmd;
      std::string out_str;
      int exit_code = Utils::shell(cmd, &out_str, true);
      LOG_TRACE << "Command: " << cmd << "\n" << out_str;

      if (exit_code != EXIT_SUCCESS) {
        LOG_ERROR << "Failed to reload the docker image and layer store, command failed: " << out_str;
        return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Could not reload docker store");
      }
    }
    LOG_INFO << "Updated docker images has been successfully enabled";
  }

  res.description += "\n# Apps installed:";

  int installed_apps_numb{0};
  for (const auto& app : Target::targetApps(target, cfg_.apps)) {
    LOG_INFO << "Installing " << app.first << " -> " << app.second;
    if (!app_ctor_(app.first).up(res.result_code == data::ResultCode::Numeric::kNeedCompletion)) {
      res = data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Could not install app: " + app.first);
      break;
    }
    res.description += "\n" + app.first + "->" + app.second;
    ++installed_apps_numb;
  };

  if (res.result_code == data::ResultCode::Numeric::kAlreadyProcessed && installed_apps_numb > 0) {
    // if the Target ostree-based rootfs is already installed and at least one of the apps was (re-)installed
    // then set the installation result code to OK
    res.result_code = data::ResultCode::Numeric::kOk;
  }

  res.description += "\n# Apps running:\n" + containerDetails();

  return res;
}

data::InstallationResult ComposeAppManager::finalizeInstall(const Uptane::Target& target) {
  auto ir = OstreeManager::finalizeInstall(target);
  if (ir.result_code != data::ResultCode::Numeric::kAlreadyProcessed ||
      ir.result_code != data::ResultCode::Numeric::kNeedCompletion) {
    ir.description += "\n# Apps running:\n" + containerDetails();
  }
  return ir;
}

void ComposeAppManager::handleRemovedApps(const Uptane::Target& target) const {
  if (!boost::filesystem::is_directory(cfg_.apps_root)) {
    LOG_DEBUG << "cfg_.apps_root does not exist";
    return;
  }

  const Target::Apps target_apps = Target::targetApps(target, cfg_.apps);

  for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(cfg_.apps_root), {})) {
    if (boost::filesystem::is_directory(entry)) {
      std::string name = entry.path().filename().native();

      if (target_apps.end() != std::find_if(target_apps.begin(), target_apps.end(),
                                            [&name](const Target::App& app) { return name == app.first; })) {
        // The App that was found on the disk is in the current Target App list
        continue;
      }

      LOG_WARNING << "Docker Compose App(" << name
                  << ") installed, "
                     "but is either removed from configuration or not defined in current Target. "
                     "Removing from system";

      app_ctor_(name).remove();
    }
  }

  if (cfg_.docker_prune) {
    LOG_INFO << "Pruning unused docker images";
    // Utils::shell which isn't interactive, we'll use std::system so that
    // stdout/stderr is streamed while docker sets things up.
    if (std::system("docker image prune -a -f --filter=\"label!=aktualizr-no-prune\"") != 0) {
      LOG_WARNING << "Unable to prune unused docker images";
    }
  }
}

std::string ComposeAppManager::getCurrentHash() const { return sysroot_->getCurDeploymentHash(); }

std::string ComposeAppManager::containerDetails() const {
  std::string cmd = cfg_.docker_bin.string();
  cmd +=
      " ps --format 'App({{.Label \"com.docker.compose.project\"}}) Service({{.Label "
      "\"com.docker.compose.service\"}} {{.Label \"io.compose-spec.config-hash\"}})'";
  std::string out_str;
  int exit_code = Utils::shell(cmd, &out_str, true);
  LOG_TRACE << "Command: " << cmd << "\n" << out_str;
  if (exit_code != EXIT_SUCCESS) {
    out_str = "Unable to run `docker ps`";
  }
  return out_str;
}
