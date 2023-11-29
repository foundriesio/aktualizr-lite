#include "cmds.h"

#include <filesystem>
#include <iostream>

#include <boost/algorithm/string.hpp>

#include "docker/composeinfo.h"
#include "docker/docker.h"
#include "docker/restorableappengine.h"
#include "http/httpclient.h"
#ifdef USE_COMPOSEAPP_ENGINE
#include "ctr/appengine.h"
#endif  // USE_COMPOSEAPP_ENGINE

namespace fs = std::filesystem;

namespace apps {
namespace aklite_apps {

struct AppDir : AppEngine::App {
  fs::path path;
};

std::vector<AppDir> getStoreApps(const std::string& store_root, const std::vector<std::string>& shortlist) {
  std::vector<AppDir> found_apps;
  const fs::path apps_dir{store_root + "/apps"};

  if (!fs::exists(store_root)) {
    LOG_INFO << "Store root directory does not exist: " << store_root;
    return found_apps;
  }

  if (!fs::exists(apps_dir)) {
    LOG_INFO << "Apps' root directory does not exist: " << apps_dir;
    return found_apps;
  }

  for (auto const& app_dir_entry : std::filesystem::directory_iterator{apps_dir}) {
    const auto app_name{app_dir_entry.path().filename().string()};

    if (shortlist.size() > 0 && shortlist.end() == std::find(shortlist.begin(), shortlist.end(), app_name)) {
      LOG_INFO << "App is not in the shortlist, skipping it: " << app_name;
      continue;
    }
    std::vector<fs::path> app_ver_dirs;
    for (auto const& app_ver_dir_entry : std::filesystem::directory_iterator{app_dir_entry.path()}) {
      app_ver_dirs.push_back(app_ver_dir_entry.path());
    }
    if (app_ver_dirs.empty()) {
      LOG_WARNING << "Haven't found any versions of App: " << app_name;
      continue;
    }
    if (app_ver_dirs.size() > 1) {
      LOG_WARNING << "Found more than one version of App: " << app_name
                  << "; number of versions: " << app_ver_dirs.size();
      LOG_WARNING << "Choosing the first found version: " << app_ver_dirs[0].filename().string();
      continue;
    }

    const auto uri_file{app_ver_dirs[0] / "uri"};
    std::string app_uri;
    if (fs::exists(uri_file)) {
      app_uri = Utils::readFile(uri_file.string());
    } else {
      // It doesn't cause any issues if running preloaded Restorable Apps.
      // As a matter of fact an app URI can be any arbitrary value.
      app_uri = "hub.foundries.io/unknown-factory/" + app_name + "@sha256:" + app_ver_dirs[0].filename().string();
      LOG_WARNING << "The App URI has not been found, assuming that the uri is: " << app_uri;
    }

    found_apps.emplace_back(AppDir{app_name, app_uri, app_ver_dirs[0]});
  }
  return found_apps;
}

int ListCmd::listApps(const std::string& store_root, bool wide) {
  std::vector<AppDir> found_apps;
  const fs::path apps_dir{store_root + "/apps"};

  if (!fs::exists(store_root)) {
    LOG_ERROR << "Store root directory does not exist: " << store_root;
    return EXIT_FAILURE;
  }

  if (!fs::exists(apps_dir)) {
    LOG_ERROR << "Apps' root directory does not exist: " << apps_dir;
    return EXIT_FAILURE;
  }

  for (auto const& app_dir_entry : std::filesystem::directory_iterator{apps_dir}) {
    const auto app_name{app_dir_entry.path().filename().string()};

    for (auto const& app_ver_dir_entry : std::filesystem::directory_iterator{app_dir_entry.path()}) {
      std::cout << app_name;
      if (wide) {
        const auto uri_file{app_ver_dir_entry.path() / "uri"};
        if (fs::exists(uri_file)) {
          const auto app_uri{Utils::readFile(uri_file.string())};
          std::cout << " --> " << app_uri;
        } else {
          std::cout << " --> hub.foundries.io/unknown-factory/" << app_name
                    << "@sha256:" << app_ver_dir_entry.path().filename().string();
        }
      }
      std::cout << std::endl;
    }
  }
  return EXIT_SUCCESS;
}

int RunCmd::runApps(const std::vector<std::string>& shortlist, const std::string& docker_host,
                    const std::string& store_root, const std::string& compose_root, const std::string& docker_root,
                    const std::string& client, const std::string& compose_client) {
  LOG_INFO << "Starting Apps preloaded into the store: " << store_root
           << "\n\tshortlist: " << boost::algorithm::join(shortlist, ",") << "\n\tdocker-host: " << docker_host
           << "\n\tcompose-root: " << compose_root << "\n\tdocker-root: " << docker_root << "\n\tclient: " << client
           << "\n\tcompose-client: " << compose_client << std::endl;

  const auto apps{getStoreApps(store_root, shortlist)};
  if (apps.size() == 0) {
    LOG_INFO << "No Apps found in the store; path:  " << store_root
             << ";  shortlist: " << boost::algorithm::join(shortlist, ",");
    exit(EXIT_SUCCESS);
  }

  auto http_client = std::make_shared<HttpClient>();
  auto docker_client{std::make_shared<Docker::DockerClient>()};
  auto registry_client{std::make_shared<Docker::RegistryClient>(http_client, "")};
#ifdef USE_COMPOSEAPP_ENGINE
  ctr::AppEngine app_engine{
      store_root,
      compose_root,
      docker_root,
      registry_client,
      docker_client,
      client,
      docker_host,
      compose_client,
      client,
      80 /* this value is non-op in the case of install/run */,
      Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
      [](const Docker::Uri& /* app_uri */, const std::string& image_uri) { return "docker://" + image_uri; },
      false,
      true};
#else
  Docker::RestorableAppEngine app_engine{
      store_root,
      compose_root,
      docker_root,
      registry_client,
      docker_client,
      client,
      docker_host,
      compose_client,
      Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
      [](const Docker::Uri& /* app_uri */, const std::string& image_uri) { return "docker://" + image_uri; },
      false,
      true};
#endif  // USE_COMPOSEAPP_ENGINE

  for (const auto& app : apps) {
    LOG_INFO << "Starting App: " << app.name;
    AppEngine::Result res = app_engine.run(app);
    if (!res) {
      LOG_ERROR << res.err;
      return static_cast<int>(res.status);
    }
  }

  LOG_INFO << "Successfully started Apps";
  return EXIT_SUCCESS;
}

}  // namespace aklite_apps
}  // namespace apps
