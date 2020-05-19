#include "composeappmanager.h"

#include "boost/process.hpp"

struct ComposeApp {
  ComposeApp(std::string name, const ComposeAppConfig &config)
      : name_(std::move(name)),
        root_(config.apps_root / name_),
        compose_(boost::filesystem::canonical(config.compose_bin).string() + " ") {}

  // Utils::shell isn't interactive. The compose commands can take a few
  // seconds to run, so we use boost::process:system to stream it to stdout/sterr
  bool cmd_streaming(const std::string &cmd) {
    LOG_DEBUG << "Running: " << cmd;
    return boost::process::system(cmd, boost::process::start_dir = root_) == 0;
  }

  bool fetch(const std::string &app_uri) {
    boost::filesystem::create_directories(root_);
    if (cmd_streaming(compose_ + "download " + app_uri)) {
      LOG_INFO << "Validating compose file";
      if (cmd_streaming(compose_ + "config")) {
        LOG_INFO << "Pulling containers";
        return cmd_streaming(compose_ + "pull");
      }
    }
    return false;
  };

  bool start() { return cmd_streaming(compose_ + "up --remove-orphans -d"); }

  void remove() {
    if (cmd_streaming(compose_ + "down")) {
      boost::filesystem::remove_all(root_);
    } else {
      LOG_ERROR << "docker-compose was unable to bring down: " << root_;
    }
  }

  std::string name_;
  boost::filesystem::path root_;
  std::string compose_;
};

ComposeAppConfig::ComposeAppConfig(const PackageConfig &pconfig) {
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
}

std::vector<std::pair<std::string, std::string>> ComposeAppManager::getApps(const Uptane::Target &t) const {
  std::vector<std::pair<std::string, std::string>> apps;

  auto target_apps = t.custom_data()["docker_compose_apps"];
  for (Json::ValueIterator i = target_apps.begin(); i != target_apps.end(); ++i) {
    if ((*i).isObject() && (*i).isMember("uri")) {
      for (const auto &app : cfg_.apps) {
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

bool ComposeAppManager::fetchTarget(const Uptane::Target &target, Uptane::Fetcher &fetcher, const KeyManager &keys,
                                    FetcherProgressCb progress_cb, const api::FlowControlToken *token) {
  if (!OstreeManager::fetchTarget(target, fetcher, keys, progress_cb, token)) {
    return false;
  }

  LOG_INFO << "Looking for Compose Apps to fetch";
  bool passed = true;
  for (const auto &pair : getApps(target)) {
    LOG_INFO << "Fetching " << pair.first << " -> " << pair.second;
    if (!ComposeApp(pair.first, cfg_).fetch(pair.second)) {
      passed = false;
    }
  }
  return passed;
}

data::InstallationResult ComposeAppManager::install(const Uptane::Target &target) const {
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
  for (const auto &pair : getApps(target)) {
    LOG_INFO << "Installing " << pair.first << " -> " << pair.second;
    if (!ComposeApp(pair.first, cfg_).start()) {
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
void ComposeAppManager::handleRemovedApps(const Uptane::Target &target) const {
  if (!boost::filesystem::is_directory(cfg_.apps_root)) {
    LOG_DEBUG << "cfg_.apps_root does not exist";
    return;
  }
  std::vector<std::string> target_apps = target.custom_data()["docker_compose_apps"].getMemberNames();

  for (auto &entry : boost::make_iterator_range(boost::filesystem::directory_iterator(cfg_.apps_root), {})) {
    if (boost::filesystem::is_directory(entry)) {
      std::string name = entry.path().filename().native();
      if (std::find(cfg_.apps.begin(), cfg_.apps.end(), name) == cfg_.apps.end()) {
        LOG_WARNING << "Docker Compose App(" << name
                    << ") installed, but is now removed from configuration. Removing from system";
        ComposeApp(name, cfg_).remove();
      } else if (std::find(target_apps.begin(), target_apps.end(), name) == target_apps.end()) {
        LOG_WARNING << "Docker Compose App(" << name
                    << ") configured, but not defined in installation target. Removing from system";
        ComposeApp(name, cfg_).remove();
      }
    }
  }
}
