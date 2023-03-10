#include "cmds.h"

#include <filesystem>
#include <iostream>

#include <boost/algorithm/string.hpp>

#include "docker/composeinfo.h"
#include "docker/docker.h"
#include "docker/restorableappengine.h"
#include "http/httpclient.h"

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

int RegisterCmd::hackDockerStore(const std::vector<std::string>& shortlist, const std::string& store_root,
                                 const std::string& docker_root) {
  LOG_INFO << "Registering the preloaded Apps at the docker store repository;"
           << "\n\tshortlist: " << boost::algorithm::join(shortlist, ",") << "\n\tstore-root: " << store_root
           << "\n\tdocker-root: " << docker_root;

  const auto apps{getStoreApps(store_root, shortlist)};
  if (apps.size() == 0) {
    LOG_INFO << "No Apps found in the store; path:  " << store_root;
    exit(EXIT_SUCCESS);
  }

  const fs::path repositories_file{docker_root + "/image/overlay2/repositories.json"};
  Json::Value repositories;
  if (fs::exists(repositories_file)) {
    repositories = Utils::parseJSONFile(repositories_file.string());
  } else {
    repositories = Utils::parseJSON("{\"Repositories\":{}}");
  }

  for (const auto& app : apps) {
    const fs::path app_compose_file{app.path / Docker::RestorableAppEngine::ComposeFile};
    const Docker::ComposeInfo app_compose{app_compose_file.string()};
    for (const Json::Value& service : app_compose.getServices()) {
      const auto image_uri_str{app_compose.getImage(service)};
      const auto image_uri{Docker::Uri::parseUri(image_uri_str, false)};

      const auto image_index_path{app.path / "images" / image_uri.registryHostname / image_uri.repo /
                                  image_uri.digest.hash() / "index.json"};
      const auto image_index{Utils::parseJSONFile(image_index_path.string())};

      // parse an image index to find a path to an image manifest
      const Docker::HashedDigest manifest_digest(image_index["manifests"][0]["digest"].asString());
      const auto image_manifest_path{store_root + "/blobs/sha256/" + manifest_digest.hash()};
      const auto image_manifest{Utils::parseJSONFile(image_manifest_path)};
      // parse an image manifest to get a digest of an image config
      const Docker::HashedDigest config_digest(image_manifest["config"]["digest"].asString());
      const auto image_repo{image_uri.registryHostname + "/" + image_uri.repo};

      LOG_INFO << "Registering image: " << image_uri_str << " -> " << config_digest();
      repositories["Repositories"][image_repo][image_uri_str] = config_digest();
    }
  }

  Utils::writeFile(repositories_file.string(), repositories);
  return EXIT_SUCCESS;
}

int RunCmd::runApps(const std::vector<std::string>& shortlist, const std::string& docker_host,
                    const std::string& store_root, const std::string& compose_root, const std::string& docker_root,
                    const std::string& client, const std::string& compose_client) {
  LOG_INFO << "Starting Apps prelaoded into the store: " << store_root
           << "\n\tshortlist: " << boost::algorithm::join(shortlist, ",") << "\n\tdocker-host: " << docker_host
           << "\n\tcompose-root: " << compose_root << "\n\tdocker-root: " << docker_root << "\n\tclient: " << client
           << "\n\tcompose-client: " << client << std::endl;

  const auto apps{getStoreApps(store_root, shortlist)};
  if (apps.size() == 0) {
    LOG_INFO << "No Apps found in the store; path:  " << store_root
             << ";  shortlist: " << boost::algorithm::join(shortlist, ",");
    exit(EXIT_SUCCESS);
  }

  auto http_client = std::make_shared<HttpClient>();
  auto docker_client{std::make_shared<Docker::DockerClient>()};
  auto registry_client{std::make_shared<Docker::RegistryClient>(http_client, "")};
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
