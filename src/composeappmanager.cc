#include "composeappmanager.h"

#include "composeapp.h"

ComposeAppManager::Config::Config(const PackageConfig& pconfig) {
  const std::map<std::string, std::string> raw = pconfig.extra;

  if (raw.count("compose_apps") == 1) {
    std::string val = raw.at("compose_apps");
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(apps, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }
  if (raw.count("compose_apps_root") == 1) {
    apps_root = raw.at("compose_apps_root");
  }
  if (raw.count("docker_compose_bin") == 1) {
    compose_bin = raw.at("docker_compose_bin");
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
      compose_bin_{boost::filesystem::canonical(cfg_.compose_bin).string() + " "} {
  for (const auto& app_name : cfg_.apps) {
    auto need_start_flag = cfg_.apps_root / app_name / Docker::ComposeApp::NeedStartFile;
    if (boost::filesystem::exists(need_start_flag)) {
      Docker::ComposeApp(app_name, cfg_.apps_root, compose_bin_, registry_client_).start();
      boost::filesystem::remove(need_start_flag);
    }
  }
}

std::vector<std::pair<std::string, std::string>> ComposeAppManager::getApps(const Uptane::Target& t) const {
  std::vector<std::pair<std::string, std::string>> apps;

  auto target_apps = t.custom_data()["docker_compose_apps"];
  for (Json::ValueIterator i = target_apps.begin(); i != target_apps.end(); ++i) {
    if ((*i).isObject() && (*i).isMember("uri")) {
      for (const auto& app : cfg_.apps) {
        if (i.key().asString() == app) {
          apps.emplace_back(app, (*i)["uri"].asString());
          break;
        }
      }
    } else {
      LOG_ERROR << "Invalid custom data for docker_compose_app: " << i.key().asString() << " -> " << *i;
    }
  }

  return apps;
}

std::vector<std::pair<std::string, std::string>> ComposeAppManager::getAppsToUpdate(const Uptane::Target& t) const {
  std::vector<std::pair<std::string, std::string>> apps_to_update;

  auto currently_installed_target_apps = OstreeManager::getCurrent().custom_data()["docker_compose_apps"];
  auto new_target_apps = getApps(t);  // intersection of apps specified in Target and the configuration

  for (const auto& app_pair : new_target_apps) {
    const auto& app_name = app_pair.first;

    auto app_data = currently_installed_target_apps.get(app_name, Json::nullValue);
    if (app_data == Json::nullValue) {
      // new app in Target
      apps_to_update.push_back(app_pair);
      LOG_INFO << app_name << " will be installed";
      continue;
    }

    if (app_pair.second != app_data["uri"].asString()) {
      // an existing App update
      apps_to_update.push_back(app_pair);
      LOG_INFO << app_name << " will be updated";
      continue;
    }

    LOG_DEBUG << "checking if " << app_name << " is installed and running...";

    if (!boost::filesystem::exists(cfg_.apps_root / app_name) ||
        !boost::filesystem::exists(cfg_.apps_root / app_name / "docker-compose.yml")) {
      // an App that is supposed to be installed has been removed somehow, let's install it again
      apps_to_update.push_back(app_pair);
      LOG_INFO << app_name << " will be re-installed";
      continue;
    }

    if (!Docker::ComposeApp(app_name, cfg_.apps_root, compose_bin_, registry_client_).isRunning()) {
      // an App that is supposed to be installed and running is not fully installed or running
      apps_to_update.push_back(app_pair);
      LOG_INFO << app_name << " update will be completed";
      continue;
    }
  }

  return apps_to_update;
}

bool ComposeAppManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  if (!OstreeManager::fetchTarget(target, fetcher, keys, progress_cb, token)) {
    return false;
  }

  if (cfg_.force_update) {
    LOG_INFO << "All Apps are forced to be updated...";
    cur_apps_to_fetch_and_update_ = getApps(target);
  } else {
    LOG_INFO << "Looking for Compose Apps to be installed or updated...";
    cur_apps_to_fetch_and_update_ = getAppsToUpdate(target);
  }

  LOG_INFO << "Found " << cur_apps_to_fetch_and_update_.size() << " Apps to update";

  bool passed = true;
  for (const auto& pair : cur_apps_to_fetch_and_update_) {
    LOG_INFO << "Fetching " << pair.first << " -> " << pair.second;
    if (!Docker::ComposeApp(pair.first, cfg_.apps_root, compose_bin_, registry_client_).fetch(pair.second)) {
      passed = false;
    }
  }
  return passed;
}

data::InstallationResult ComposeAppManager::install(const Uptane::Target& target) const {
  data::InstallationResult res;
  Uptane::Target current = OstreeManager::getCurrent();
  if (current.sha256Hash() != target.sha256Hash()) {
    // notify the bootloader before installation happens as it is not atomic
    // and a false notification doesn't hurt with rollback support in place
    updateNotify();
    res = OstreeManager::install(target);
    if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
      LOG_ERROR << "Failed to install OSTree target, skipping Docker Compose Apps";
      return res;
    }
  } else {
    LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
    res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
  }

  handleRemovedApps(target);

  // make sure we install what we fecthed
  for (const auto& pair : cur_apps_to_fetch_and_update_) {
    LOG_INFO << "Installing " << pair.first << " -> " << pair.second;
    if (!Docker::ComposeApp(pair.first, cfg_.apps_root, compose_bin_, registry_client_)
             .up(res.result_code == data::ResultCode::Numeric::kNeedCompletion)) {
      res = data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Could not install app");
    }
  };

  if (cfg_.docker_prune) {
    LOG_INFO << "Pruning unused docker images";
    // Utils::shell which isn't interactive, we'll use std::system so that
    // stdout/stderr is streamed while docker sets things up.
    if (std::system("docker image prune -a -f --filter=\"label!=aktualizr-no-prune\"") != 0) {
      LOG_WARNING << "Unable to prune unused docker images";
    }
  }

  return res;
}

// Handle the case like:
//  1) sota.toml is configured with 2 compose apps: "app1, app2"
//  2) update is applied, so we are now running both app1 and app2
//  3) sota.toml is updated with 1 docker app: "app1"
// At this point we should stop app2 and remove it.
void ComposeAppManager::handleRemovedApps(const Uptane::Target& target) const {
  if (!boost::filesystem::is_directory(cfg_.apps_root)) {
    LOG_DEBUG << "cfg_.apps_root does not exist";
    return;
  }
  std::vector<std::string> target_apps = target.custom_data()["docker_compose_apps"].getMemberNames();

  for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(cfg_.apps_root), {})) {
    if (boost::filesystem::is_directory(entry)) {
      std::string name = entry.path().filename().native();
      if (std::find(cfg_.apps.begin(), cfg_.apps.end(), name) == cfg_.apps.end()) {
        LOG_WARNING << "Docker Compose App(" << name
                    << ") installed, but is now removed from configuration. Removing from system";
        Docker::ComposeApp(name, cfg_.apps_root, compose_bin_, registry_client_).remove();
      } else if (std::find(target_apps.begin(), target_apps.end(), name) == target_apps.end()) {
        LOG_WARNING << "Docker Compose App(" << name
                    << ") configured, but not defined in installation target. Removing from system";
        Docker::ComposeApp(name, cfg_.apps_root, compose_bin_, registry_client_).remove();
      }
    }
  }
}

std::string ComposeAppManager::getCurrentHash() const { return sysroot_->getCurDeploymentHash(); }
