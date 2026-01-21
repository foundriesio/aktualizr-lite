#include "appengine.h"

#include <boost/format.hpp>

#include "aktualizr-lite/storage/stat.h"
#include "exec.h"

namespace composeapp {
enum class ExitCode { ExitCodeInsufficientSpace = 100 };

static bool checkAppStatus(const AppEngine::App& app, const Json::Value& status);
static bool checkAppInstallationStatus(const AppEngine::App& app, const Json::Value& status);
static bool isNullOrEmptyOrUnset(const Json::Value& val, const std::string& field);

AppEngine::Result AppEngine::fetch(const App& app) {
  Result res{false};
  bool was_proxy_set{false};
  try {
    // If a given app was fetched before, then don't consider it as a fetched app if a caller tries to fetch it again
    // for one reason or another - hence remove it from the set of fetched apps.
    fetched_apps_.erase(app.uri);
    if (local_source_path_.empty()) {
      if (proxy_) {
        // If the proxy provider is set, then obtain the proxy URL and CA from it,
        // and set the corresponding environment variables for `composectl`.
        const auto proxy{proxy_()};
        if (!proxy.first.empty()) {
          ::setenv("COMPOSE_APPS_PROXY", proxy.first.c_str(), 1);
          ::setenv("COMPOSE_APPS_PROXY_CA", proxy.second.c_str(), 1);
          was_proxy_set = true;
        }
      }
      exec(boost::format{"%s --store %s pull -p %s --storage-usage-watermark %d"} % composectl_cmd_ % storeRoot() %
               app.uri % storage_watermark_,
           "failed to pull compose app", "", nullptr, "4h", true);
    } else {
      exec(boost::format{"%s --store %s pull -p %s -l %s --storage-usage-watermark %d"} % composectl_cmd_ %
               storeRoot() % app.uri % local_source_path_ % storage_watermark_,
           "failed to pull compose app", "", nullptr, "4h", true);
    }
    res = true;
    fetched_apps_.insert(app.uri);
  } catch (const ExecError& exc) {
    if (exc.ExitCode == static_cast<int>(ExitCode::ExitCodeInsufficientSpace)) {
      const auto usage_stat{Utils::parseJSON(exc.StdErr)};
      auto usage_info{storageSpaceFunc()(usage_stat["path"].asString())};
      res = {Result::ID::InsufficientSpace, exc.what(), usage_info.withRequired(usage_stat["required"].asUInt64())};
    } else {
      res = {false, exc.what()};
    }
  } catch (const std::exception& exc) {
    res = {false, exc.what()};
  }
  if (was_proxy_set) {
    ::unsetenv("COMPOSE_APPS_PROXY");
    ::unsetenv("COMPOSE_APPS_PROXY_CA");
  }
  return res;
}

void AppEngine::remove(const App& app) {
  try {
    fetched_apps_.erase(app.uri);
    // "App removal" in this context refers to deleting app images from the Docker store
    // and removing the app compose project (app uninstall).
    // Unused app blobs will be removed from the blob store via the prune() method,
    // provided they are not utilized by any other app(s).
    // Note: Ensure the app is stopped before attempting to uninstall it.

    exec(boost::format{"%s --store %s --compose %s stop %s"} % composectl_cmd_ % storeRoot() % installRoot() % app.name,
         "failed to stop app");
    // Uninstall app, it only removes the app compose/project directory, docker store pruning is in the `prune` call
    exec(boost::format{"%s --store %s --compose %s uninstall --ignore-non-installed %s"} % composectl_cmd_ %
             storeRoot() % installRoot() % app.name,
         "failed to uninstall app");
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", failed to remove: " << exc.what();
  }
}

bool AppEngine::isRunning(const App& app) const {
  bool res{false};
  try {
    std::string output;
    exec(boost::format{"%s --store %s --compose %s ps %s --format json"} % composectl_cmd_ % storeRoot() %
             installRoot() % app.uri,
         "", "", &output, "900s", false, true);
    const auto app_status{Utils::parseJSON(output)};
    // Make sure app images and bundle are properly installed
    res = checkAppInstallationStatus(app, app_status);
    if (res) {
      // Make sure app is running
      res = checkAppStatus(app, app_status);
    }
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify whether app is running; app: " << app.name << ", err: " << exc.what();
  }
  return res;
}

Json::Value AppEngine::getRunningAppsInfo() const {
  Json::Value app_statuses;
  try {
    std::string output;
    exec(boost::format{"%s --store %s ps --format json"} % composectl_cmd_ % storeRoot(), "", "", &output, "900s",
         false, true);
    app_statuses = Utils::parseJSON(output);
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get an info about running containers: " << exc.what();
  }

  return app_statuses;
}

void AppEngine::prune(const Apps& app_shortlist) {
  try {
    // Remove apps that are not in the shortlist
    std::string output;
    exec(boost::format{"%s --store %s ls --format json"} % composectl_cmd_ % storeRoot(), "failed to list apps", "",
         &output, "900s", false, true);
    const auto app_list{Utils::parseJSON(output)};

    Apps apps_to_prune;
    for (const auto& store_app_json : app_list) {
      if (!(store_app_json.isMember("name") && store_app_json.isMember("uri"))) {
        continue;
      }

      bool is_in_shortlist{false};
      App store_app{store_app_json["name"].asString(), store_app_json["uri"].asString()};
      for (const auto& shortlisted_app : app_shortlist) {
        if (store_app == shortlisted_app) {
          is_in_shortlist = true;
          break;
        }
      }
      if (!is_in_shortlist) {
        apps_to_prune.push_back(store_app);
      }
    }
    for (const auto& app : apps_to_prune) {
      fetched_apps_.erase(app.uri);
      exec(boost::format{"%s --store %s rm %s --prune=false --quiet"} % composectl_cmd_ % storeRoot() % app.uri,
           "failed to remove app");
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to remove unused apps: " << exc.what();
  }
  try {
    // Pruning unused store blobs
    std::string output;
    exec(boost::format{"%s --store %s prune --format=json"} % composectl_cmd_ % storeRoot(),
         "failed to prune app blobs", "", &output, "900s", false, true);
    const auto pruned_blobs{Utils::parseJSON(output)};

    // If at least one blob was pruned then the docker store needs to be pruned too to remove corresponding blobs
    // from the docker store
    if (!pruned_blobs.isNull() && !pruned_blobs.empty()) {
      LOG_INFO << "Pruning unused docker containers";
      dockerClient()->pruneContainers();
      LOG_INFO << "Pruning unused docker images";
      dockerClient()->pruneImages();
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to remove unused apps: " << exc.what();
  }
}

bool AppEngine::isAppFetched(const App& app) const {
  bool res{false};
  if (fetched_apps_.count(app.uri) > 0) {
    return true;
  }
  try {
    std::string output;
    exec(boost::format{"%s --store %s check %s --local --format json"} % composectl_cmd_ % storeRoot() % app.uri, "",
         "", &output, "900s", false, true);
    const auto app_fetch_status{Utils::parseJSON(output)};
    if (app_fetch_status.isMember("fetch_check") && app_fetch_status["fetch_check"].isMember("missing_blobs") &&
        app_fetch_status["fetch_check"]["missing_blobs"].empty()) {
      res = true;
      fetched_apps_.insert(app.uri);
    }
  } catch (const ExecError& exc) {
    LOG_DEBUG << "app is not fully fetched; app: " << app.name << ", status: " << exc.what();
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify whether app is fetched; app: " << app.name << ", err: " << exc.what();
    throw;
  }
  return res;
}

bool AppEngine::isAppInstalled(const App& app) const {
  bool res{false};
  try {
    std::string output;
    exec(boost::format{"%s --store %s check %s --local --install --format json"} % composectl_cmd_ % storeRoot() %
             app.uri,
         "", "", &output, "900s", false, true);
    const auto app_fetch_status{Utils::parseJSON(output)};
    if (app_fetch_status.isMember("install_check") && app_fetch_status["install_check"].isMember(app.uri) &&
        app_fetch_status["install_check"][app.uri].isMember("missing_images") &&
        (app_fetch_status["install_check"][app.uri]["missing_images"].isNull() ||
         app_fetch_status["install_check"][app.uri]["missing_images"].empty())) {
      res = true;
    }
  } catch (const ExecError& exc) {
    LOG_DEBUG << "app is not fully fetched or installed; app: " << app.name << ", status: " << exc.what();
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify whether app is installed; app: " << app.name << ", err: " << exc.what();
    throw;
  }
  return res;
}

void AppEngine::installAppAndImages(const App& app) {
  exec(boost::format{"%s --store %s --compose %s --host %s install %s"} % composectl_cmd_ % storeRoot() %
           installRoot() % dockerHost() % app.uri,
       "failed to install compose app", "", nullptr, "4h", true);
}

static bool checkAppStatus(const AppEngine::App& app, const Json::Value& status) {
  if (!status.isMember(app.uri)) {
    LOG_ERROR << "could not get app status; uri: " << app.uri;
    return false;
  }
  if (!status[app.uri].isMember("services") || status[app.uri]["services"].isNull()) {
    LOG_INFO << app.name << " is not running; uri: " << app.uri;
    return false;
  }

  bool is_running{true};
  const std::set<std::string> broken_states{"created", "missing", "unknown"};
  for (const auto& s : status[app.uri]["services"]) {
    if (broken_states.count(s["state"].asString()) > 0) {
      is_running = false;
      break;
    }
  }
  if (!is_running) {
    LOG_INFO << app.name << " is not running; uri: " << app.uri;
    LOG_INFO << status[app.uri];
  }
  return is_running;
}

static bool checkAppInstallationStatus(const AppEngine::App& app, const Json::Value& status) {
  const auto& app_status{status.get(app.uri, Json::Value())};
  if (!app_status.isObject()) {
    LOG_ERROR << "could not get app status; uri: " << app.uri;
    return false;
  }
  if (isNullOrEmptyOrUnset(app_status, "in_store")) {
    LOG_ERROR << "could not check if app is in store; uri: " << app.uri;
    return false;
  }
  if (!app_status["in_store"].asBool()) {
    LOG_INFO << app.name << " is not found in the local store";
    return false;
  }
  if (!isNullOrEmptyOrUnset(app_status, "missing_images")) {
    LOG_INFO << app.name << " is not fully installed; missing images:\n" << app_status["missing_images"];
    return false;
  }
  if (!isNullOrEmptyOrUnset(app_status, "bundle_errors")) {
    LOG_INFO << app.name << " is not fully installed; invalid bundle installation:\n" << app_status["bundle_errors"];
    return false;
  }
  return true;
}

static bool isNullOrEmptyOrUnset(const Json::Value& val, const std::string& field) {
  bool res{false};
  if (val.isMember(field)) {
    res = val[field].isNull() || val[field].empty();
  } else {
    res = true;
  }
  return res;
}

}  // namespace composeapp
