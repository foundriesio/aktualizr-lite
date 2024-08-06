#include "appengine.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "exec.h"
#include "storage/stat.h"

namespace composeapp {
enum class ExitCode { ExitCodeInsufficientSpace = 100 };

static bool checkAppStatus(const AppEngine::App& app, const Json::Value& status);

AppEngine::Result AppEngine::fetch(const App& app) {
  Result res{false};
  try {
    if (local_source_path_.empty()) {
      exec(boost::format{"%s --store %s pull -p %s --storage-usage-watermark %d"} % composectl_cmd_ % storeRoot() %
               app.uri % storage_watermark_,
           "failed to pull compose app");
    } else {
      exec(boost::format{"%s --store %s pull -p %s -l %s --storage-usage-watermark %d"} % composectl_cmd_ %
               storeRoot() % app.uri % local_source_path_ % storage_watermark_,
           "failed to pull compose app");
    }
    res = true;
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
  return res;
}

void AppEngine::remove(const App& app) {
  try {
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
    std::future<std::string> output;
    exec(boost::format{"%s --store %s ps %s --format json"} % composectl_cmd_ % storeRoot() % app.uri, "",
         boost::process::std_out > output);
    const auto app_status{Utils::parseJSON(output.get())};
    res = checkAppStatus(app, app_status);
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify whether app is running; app: " << app.name << ", err: " << exc.what();
  }
  return res;
}

void AppEngine::prune(const Apps& app_shortlist) {
  try {
    // Remove apps that are not in the shortlist
    std::future<std::string> output;
    exec(boost::format{"%s --store %s ls --format json"} % composectl_cmd_ % storeRoot(), "failed to list apps",
         boost::process::std_out > output);
    const std::string output_str{output.get()};
    const auto app_list{Utils::parseJSON(output_str)};

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
      exec(boost::format{"%s --store %s rm %s --prune=false --quiet"} % composectl_cmd_ % storeRoot() % app.uri,
           "failed to remove app");
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to remove unused apps: " << exc.what();
  }
  try {
    // Pruning unused store blobs
    std::future<std::string> output;
    exec(boost::format{"%s --store %s prune --format=json"} % composectl_cmd_ % storeRoot(),
         "failed to prune app blobs", boost::process::std_out > output);
    const std::string output_str{output.get()};
    const auto pruned_blobs{Utils::parseJSON(output_str)};

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
  try {
    std::future<std::string> output;
    exec(boost::format{"%s --store %s check %s --local --format json"} % composectl_cmd_ % storeRoot() % app.uri, "",
         boost::process::std_out > output);
    const std::string output_str{output.get()};
    const auto app_fetch_status{Utils::parseJSON(output_str)};
    if (app_fetch_status.isMember("fetch_check") && app_fetch_status["fetch_check"].isMember("missing_blobs") &&
        app_fetch_status["fetch_check"]["missing_blobs"].empty()) {
      res = true;
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
    std::future<std::string> output;
    exec(boost::format{"%s --store %s check %s --local --install --format json"} % composectl_cmd_ % storeRoot() %
             app.uri,
         "", boost::process::std_out > output);
    const std::string output_str{output.get()};
    const auto app_fetch_status{Utils::parseJSON(output_str)};
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
       "failed to installl compose app");
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

}  // namespace composeapp
