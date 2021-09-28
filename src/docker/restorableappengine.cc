#include "restorableappengine.h"

#include <unordered_set>

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"
#include "docker/composeappengine.h"
#include "docker/composeinfo.h"

namespace Docker {

const std::string RestorableAppEngine::ComposeFile{"docker-compose.yml"};

template <typename... Args>
static void exec(const boost::format& cmd, const std::string& err_msg, Args&&... args);

RestorableAppEngine::RestorableAppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
                                         Docker::RegistryClient::Ptr registry_client,
                                         Docker::DockerClient::Ptr docker_client, std::string client,
                                         std::string docker_host, std::string compose_cmd)
    : store_root_{std::move(store_root)},
      install_root_{std::move(install_root)},
      client_{std::move(client)},
      docker_host_{std::move(docker_host)},
      compose_cmd_{std::move(compose_cmd)},
      registry_client_{std::move(registry_client)},
      docker_client_{std::move(docker_client)} {
  boost::filesystem::create_directories(apps_root_);
  boost::filesystem::create_directories(blobs_root_);
}

bool RestorableAppEngine::fetch(const App& app) {
  bool res{false};
  try {
    const Uri uri{Uri::parseUri(app.uri)};
    const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
    const auto app_compose_file{app_dir / ComposeFile};

    if (!isAppFetched(app)) {
      LOG_INFO << app.name << ": downloading App from Registry: " << app.uri << " --> " << app_dir;
      pullApp(uri, app_dir);
    } else {
      LOG_INFO << app.name << ": App already fetched: " << app_dir;
    }

    // Invoke download of App images unconditionally because `skopeo` is supposed
    // to skip already downloaded image blobs internally while performing `copy` command
    const auto images_dir{app_dir / "images"};
    LOG_DEBUG << app.name << ": downloading App images from Registry(ies): " << app.uri << " --> " << images_dir;
    pullAppImages(app_compose_file, images_dir);
    res = true;
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to fetch App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
  }

  return res;
}

bool RestorableAppEngine::verify(const App& app) {
  bool res{false};
  try {
    const Uri uri{Uri::parseUri(app.uri)};
    const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
    TemporaryDirectory app_tmp_dir;
    installApp(app_dir, app_tmp_dir.Path());
    LOG_DEBUG << app.name << ": verifying App: " << app_dir << " --> " << app_tmp_dir.Path();
    res = EXIT_SUCCESS ==
          boost::process::system(compose_cmd_ + " config -q", boost::process::start_dir = app_tmp_dir.Path());
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
  }
  return res;
}

bool RestorableAppEngine::install(const App& app) {
  bool res{false};
  try {
    const auto app_install_root{installAppAndImages(app)};
    startComposeApp(compose_cmd_, app_install_root, "--remove-orphans --no-start");
    res = true;
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to install App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
  }
  return res;
}

bool RestorableAppEngine::run(const App& app) {
  bool res{false};
  try {
    const auto app_install_root{installAppAndImages(app)};
    startComposeApp(compose_cmd_, app_install_root, "--remove-orphans -d");
    res = true;
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to start App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
  }
  return res;
}

void RestorableAppEngine::remove(const App& app) {
  try {
    const auto app_install_dir{install_root_ / app.name};

    // just installed app are removed, the restorable store Apps will be removed by means of prune() call
    stopComposeApp(compose_cmd_, app_install_dir);
    boost::filesystem::remove_all(app_install_dir);

  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", failed to remove: " << exc.what();
  }
}

bool RestorableAppEngine::isFetched(const App& app) const {
  bool res{false};
  try {
    res = isAppFetched(app);
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", cannot check whether App is fetched: " << exc.what();
  }
  return res;
}

bool RestorableAppEngine::isRunning(const App& app) const {
  bool res{false};

  try {
    res = isAppFetched(app) && isAppInstalled(app) &&
          isRunning(app, (install_root_ / app.name / ComposeFile).string(), docker_client_);
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", cannot check whether App is running: " << exc.what();
  }

  return res;
}

Json::Value RestorableAppEngine::getRunningAppsInfo() const {
  Json::Value apps;
  try {
    Json::Value containers;
    docker_client_->getContainers(containers);

    for (Json::ValueIterator ii = containers.begin(); ii != containers.end(); ++ii) {
      Json::Value val = *ii;

      std::string app_name = val["Labels"]["com.docker.compose.project"].asString();
      if (app_name.empty()) {
        continue;
      }

      std::string service = val["Labels"]["com.docker.compose.service"].asString();
      std::string hash = val["Labels"]["io.compose-spec.config-hash"].asString();
      std::string image = val["Image"].asString();
      std::string state = val["State"].asString();
      std::string status = val["Status"].asString();

      apps[app_name]["services"][service]["hash"] = hash;
      apps[app_name]["services"][service]["image"] = image;
      apps[app_name]["services"][service]["state"] = state;
      apps[app_name]["services"][service]["status"] = status;
    }

  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get an info about running containers: " << exc.what();
  }

  return apps;
}

void RestorableAppEngine::prune(const Apps& app_shortlist) {
  std::unordered_set<std::string> blob_shortlist;
  bool prune_docker_store{false};

  for (const auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(apps_root_), {})) {
    if (!boost::filesystem::is_directory(entry)) {
      continue;
    }
    const std::string dir = entry.path().filename().native();
    auto foundAppIt = std::find_if(app_shortlist.begin(), app_shortlist.end(),
                                   [&dir](const AppEngine::App& app) { return dir == app.name; });

    if (foundAppIt == app_shortlist.end()) {
      // remove App dir tree since it's not found in the shortlist
      boost::filesystem::remove_all(entry.path());
      LOG_INFO << "Removing App dir: " << entry.path();
      prune_docker_store = true;
      continue;
    }

    const auto& app{*foundAppIt};
    const Uri uri{Uri::parseUri(app.uri)};

    // iterate over `app` subdirectories/versions and remove those that doesn't match the specified version
    const auto app_dir{apps_root_ / uri.app};

    for (const auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(app_dir), {})) {
      if (!boost::filesystem::is_directory(entry)) {
        LOG_WARNING << "Found file while expected an App version directory: " << entry.path().filename().native();
        continue;
      }

      const std::string app_version_dir = entry.path().filename().native();
      if (app_version_dir != uri.digest.hash()) {
        LOG_INFO << "Removing App version dir: " << entry.path();
        boost::filesystem::remove_all(entry.path());
        prune_docker_store = true;
        continue;
      }

      // add blobs of the shortlisted apps to the blob shortlist
      ComposeInfo compose{(entry.path() / ComposeFile).string()};
      for (const auto& service : compose.getServices()) {
        const auto image = compose.getImage(service);
        const Uri image_uri{Uri::parseUri(image)};
        const auto image_root{app_dir / app_version_dir / "images" / image_uri.registryHostname / image_uri.repo /
                              image_uri.digest.hash()};

        const auto image_manifest_desc{Utils::parseJSONFile(image_root / "index.json")};
        HashedDigest image_digest{image_manifest_desc["manifests"][0]["digest"].asString()};
        blob_shortlist.emplace(image_digest.hash());

        const auto image_manifest{Utils::parseJSONFile(blobs_root_ / "sha256" / image_digest.hash())};
        blob_shortlist.emplace(HashedDigest(image_manifest["config"]["digest"].asString()).hash());

        const auto image_layers{image_manifest["layers"]};
        for (Json::ValueConstIterator ii = image_layers.begin(); ii != image_layers.end(); ++ii) {
          if ((*ii).isObject() && (*ii).isMember("digest")) {
            const auto layer_digest{HashedDigest{(*ii)["digest"].asString()}};
            blob_shortlist.emplace(layer_digest.hash());
          } else {
            LOG_ERROR << "Invalid image manifest: " << ii.key().asString() << " -> " << *ii;
          }
        }
      }
    }
  }

  // prune blobs
  if (!boost::filesystem::exists(blobs_root_ / "sha256")) {
    return;
  }

  for (const auto& entry :
       boost::make_iterator_range(boost::filesystem::directory_iterator(blobs_root_ / "sha256"), {})) {
    if (boost::filesystem::is_directory(entry)) {
      continue;
    }

    const std::string blob_sha = entry.path().filename().native();
    if (blob_shortlist.end() == blob_shortlist.find(blob_sha)) {
      LOG_INFO << "Removing blob: " << entry.path();
      boost::filesystem::remove_all(entry.path());
      prune_docker_store = true;
    }
  }

  // prune docker store
  if (prune_docker_store) {
    ComposeAppEngine::pruneDockerStore();
  }
}

// protected & private implementation

void RestorableAppEngine::pullApp(const Uri& uri, const boost::filesystem::path& app_dir) {
  boost::filesystem::create_directories(app_dir);

  const std::string manifest_str{registry_client_->getAppManifest(uri, Manifest::Format)};
  const Manifest manifest{manifest_str};
  Docker::Uri archive_uri{uri.createUri(manifest.archiveDigest())};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  registry_client_->downloadBlob(archive_uri, archive_full_path, manifest.archiveSize());
  Utils::writeFile(app_dir / Manifest::Filename, manifest_str);
  // extract docker-compose.yml, temporal hack, we don't need to extract it
  exec(boost::format{"tar -xzf %s %s"} % archive_full_path % ComposeFile, "failed to extract the compose app archive",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::pullAppImages(const boost::filesystem::path& app_compose_file,
                                        const boost::filesystem::path& dst_dir) {
  // REGISTRY_AUTH_FILE env. var. must be set and point to the docker's `config.json` (e.g.
  // /usr/lib/docker/config.json)`
  // {
  //  "credHelpers": {
  //    "hub.foundries.io": "fio-helper"
  //  }
  // }
  //
  // `"hub.foundries.io": "fio-helper"` implies that there is `/usr/bin/docker-credential-fio-helper` which returns
  // creds access to customer specific registry can be provided in the same way, i.e. defining the registry specific
  // cred helper.
  boost::filesystem::create_directories(dst_dir);

  const auto compose{ComposeInfo(app_compose_file.string())};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);

    const Uri uri{Uri::parseUri(image_uri)};
    const auto image_dir{dst_dir / uri.registryHostname / uri.repo / uri.digest.hash()};

    LOG_INFO << uri.app << ": downloading image from Registry if missing: " << image_uri << " --> " << dst_dir;
    pullImage(client_, image_uri, image_dir, blobs_root_);
  }
}

boost::filesystem::path RestorableAppEngine::installAppAndImages(const App& app) {
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  auto app_install_dir{install_root_ / app.name};
  LOG_DEBUG << app.name << ": installing App: " << app_dir << " --> " << app_install_dir;
  installApp(app_dir, app_install_dir);
  LOG_DEBUG << app.name << ": verifying App: " << app_install_dir;
  verifyComposeApp(compose_cmd_, app_install_dir);
  LOG_DEBUG << app.name << ": installing App images: " << app_dir << " --> docker-daemon://";
  installAppImages(app_dir);
  return app_install_dir;
}

void RestorableAppEngine::installApp(const boost::filesystem::path& app_dir, const boost::filesystem::path& dst_dir) {
  const Manifest manifest{Utils::parseJSONFile(app_dir / Manifest::Filename)};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  boost::filesystem::create_directories(dst_dir);
  exec(boost::format{"tar --overwrite -xzf %s"} % archive_full_path.string(), "failed to install Compose App",
       boost::process::start_dir = dst_dir);
}

void RestorableAppEngine::installAppImages(const boost::filesystem::path& app_dir) {
  const auto compose{ComposeInfo((app_dir / ComposeFile).string())};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);
    const Uri uri{Uri::parseUri(image_uri)};
    const std::string tag{uri.registryHostname + '/' + uri.repo + ':' + uri.digest.shortHash()};
    const auto image_dir{app_dir / "images" / uri.registryHostname / uri.repo / uri.digest.hash()};
    installImage(client_, image_dir, blobs_root_, docker_host_, tag);
  }
}

bool RestorableAppEngine::isAppFetched(const App& app) const {
  bool res{false};
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};

  do {
    if (!boost::filesystem::exists(app_dir)) {
      LOG_DEBUG << app.name << ": missing App dir: " << app_dir;
      break;
    }

    const auto manifest_file{app_dir / Manifest::Filename};
    if (!boost::filesystem::exists(manifest_file)) {
      LOG_DEBUG << app.name << ": missing App manifest: " << manifest_file;
      break;
    }

    const auto manifest_str{Utils::readFile(manifest_file)};
    const auto manifest_hash =
        boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(manifest_str)));

    if (manifest_hash != uri.digest.hash()) {
      LOG_DEBUG << app.name << ": App manifest hash mismatch; actual: " << manifest_hash
                << "; expected: " << uri.digest.hash();
      break;
    }

    const Manifest manifest{Utils::parseJSON(manifest_str)};

    // verify App archive/blob hash
    const auto archive_manifest_hash{HashedDigest(manifest.archiveDigest()).hash()};
    const auto archive_full_path{app_dir / (archive_manifest_hash + Manifest::ArchiveExt)};
    if (!boost::filesystem::exists(archive_full_path)) {
      LOG_DEBUG << app.name << ": missing App archive: " << archive_full_path;
      break;
    }

    // we assume that a compose App blob is relatively small so we can just read it all into RAM
    const auto app_arch_str = Utils::readFile(archive_full_path);
    const auto app_arch_hash =
        boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(app_arch_str)));
    if (app_arch_hash != archive_manifest_hash) {
      LOG_DEBUG << app.name << ": App archive hash mismatch; actual: " << app_arch_str
                << "; defined in manifest: " << archive_manifest_hash;
      break;
    }

    // No need to check hashes of a Merkle tree of each App image
    // since skopeo does it internally within in the `skopeo copy` command

    res = true;
  } while (false);

  return res;
}

bool RestorableAppEngine::isAppInstalled(const App& app) const {
  bool res{false};
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  const auto app_install_dir{install_root_ / app.name};

  do {
    const auto manifest_file{app_dir / Manifest::Filename};
    const Manifest manifest{Utils::parseJSONFile(manifest_file)};
    const auto archive_manifest_hash{HashedDigest(manifest.archiveDigest()).hash()};
    const auto archive_full_path{app_dir / (archive_manifest_hash + Manifest::ArchiveExt)};

    exec(boost::format{"tar -xzf %s %s"} % archive_full_path % ComposeFile,
         app.name + ": failed to extract a compose file from the compose app archive",
         boost::this_process::environment(), boost::process::start_dir = app_dir);

    const auto compose_file_str = Utils::readFile(app_dir / ComposeFile);
    const auto compose_file_hash =
        boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(compose_file_str)));
    const auto installed_compose_file_str = Utils::readFile(app_install_dir / ComposeFile);
    const auto installed_compose_file_hash =
        boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(installed_compose_file_str)));

    if (compose_file_hash != installed_compose_file_hash) {
      LOG_DEBUG << app.name << "; a compose file hash mismatch; installed: " << installed_compose_file_hash
                << "fetched: " << compose_file_hash;
      break;
    }
    // TODO: check whether docker store has all App images

    res = true;
  } while (false);

  return res;
}

bool RestorableAppEngine::isRunning(const App& app, const std::string& compose_file,
                                    const Docker::DockerClient::Ptr& docker_client) {
  ComposeInfo compose{compose_file};
  std::vector<Json::Value> services = compose.getServices();

  if (services.empty()) {
    LOG_WARNING << "App: " << app.name << ", no services in App's compose file: " << compose_file;
    return false;
  }

  Json::Value containers;
  docker_client->getContainers(containers);

  for (std::size_t i = 0; i < services.size(); i++) {
    std::string service = services[i].asString();
    std::string hash = compose.getHash(services[i]);
    if (docker_client->isRunning(containers, app.name, service, hash)) {
      continue;
    }
    LOG_WARNING << "App: " << app.name << ", service: " << service << ", hash: " << hash << ", not running!";
    return false;
  }

  return true;
}

// static methods to manage image data and Compose App

void RestorableAppEngine::pullImage(const std::string& client, const std::string& uri,
                                    const boost::filesystem::path& dst_dir,
                                    const boost::filesystem::path& shared_blob_dir, const std::string& format) {
  boost::filesystem::create_directories(dst_dir);
  exec(boost::format{"%s copy -f %s --dest-shared-blob-dir %s docker://%s oci:%s"} % client % format %
           shared_blob_dir.string() % uri % dst_dir.string(),
       "failed to pull image", boost::this_process::environment());
}

void RestorableAppEngine::installImage(const std::string& client, const boost::filesystem::path& image_dir,
                                       const boost::filesystem::path& shared_blob_dir, const std::string& docker_host,
                                       const std::string& tag, const std::string& format) {
  exec(boost::format{"%s copy -f %s --dest-daemon-host %s --src-shared-blob-dir %s oci:%s docker-daemon:%s"} % client %
           format % docker_host % shared_blob_dir.string() % image_dir.string() % tag,
       "failed to install image", boost::this_process::environment());
}

void RestorableAppEngine::verifyComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir) {
  exec(boost::format{"%s config"} % compose_cmd, "Compose App verification failed",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::startComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir,
                                          const std::string& flags) {
  exec(boost::format{"%s up %s"} % compose_cmd % flags, "failed to bring Compose App up",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::stopComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir) {
  exec(boost::format{"%s down"} % compose_cmd, "failed to bring Compose App down", boost::process::start_dir = app_dir);
}

template <typename... Args>
void exec(const boost::format& cmd, const std::string& err_msg, Args&&... args) {
  if (EXIT_SUCCESS != boost::process::system(cmd.str(), std::forward<Args>(args)...)) {
    throw std::runtime_error(err_msg + "; cmd: " + cmd.str());
  }
}

}  // namespace Docker
