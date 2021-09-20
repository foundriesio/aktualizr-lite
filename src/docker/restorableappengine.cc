#include "restorableappengine.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"
#include "docker/composeinfo.h"

namespace Docker {

const std::string RestorableAppEngine::ComposeFile{"docker-compose.yml"};

RestorableAppEngine::RestorableAppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
                                         Docker::RegistryClient::Ptr registry_client,
                                         Docker::DockerClient::Ptr docker_client, const std::string& client,
                                         const std::string& docker_host, const std::string& compose_cmd)
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
    LOG_DEBUG << app.name << ": downloading App from Registry: " << app.uri << " --> " << app_dir;
    const auto app_compose_file{pullApp(uri, app_dir)};

    const auto images_dir{app_dir / "images"};
    LOG_DEBUG << app.name << ": downloading App images from Registry(ies): " << app.uri << " --> " << images_dir;
    pullAppImages(app_compose_file, images_dir);
    res = true;
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to fetch App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
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

void RestorableAppEngine::remove(const App& app) {}

bool RestorableAppEngine::isRunning(const App& app) const {
  bool res{false};

  try {
    res = isAppInstalled(app) && isRunning(app, (install_root_ / app.name / ComposeFile).string(), docker_client_);
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", cant check whether App is running: " << exc.what();
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

// protected & private implementation

boost::filesystem::path RestorableAppEngine::pullApp(const Uri& uri, const boost::filesystem::path& app_dir) {
  boost::filesystem::create_directories(app_dir);

  const Manifest manifest{registry_client_->getAppManifest(uri, Manifest::Format)};
  Docker::Uri archive_uri{uri.createUri(manifest.archiveDigest())};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  registry_client_->downloadBlob(archive_uri, archive_full_path, manifest.archiveSize());
  // TODO: verify archive

  Utils::writeFile(app_dir / Manifest::Filename, manifest);
  // extract docker-compose.yml, temporal hack, we don't need to extract it
  auto cmd = boost::str(boost::format("tar -xzf %s %s") % archive_full_path % ComposeFile);
  if (0 != boost::process::system(cmd, boost::this_process::environment(), boost::process::start_dir = app_dir)) {
    throw std::runtime_error("failed to extract the compose app archive: " + archive_full_path.string());
  }

  return app_dir / ComposeFile;
}

void RestorableAppEngine::pullAppImages(const boost::filesystem::path& app_compose_file,
                                        const boost::filesystem::path& dst_dir) {
  boost::filesystem::create_directories(dst_dir);

  const auto compose{ComposeInfo(app_compose_file.string())};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);
    LOG_ERROR << image_uri;

    const Uri uri{Uri::parseUri(image_uri)};
    const auto image_dir{dst_dir / uri.registryHostname / uri.repo / uri.digest.hash()};

    LOG_DEBUG << uri.app << ": downloading image from Registry: " << image_uri << " --> " << dst_dir;
    pullImage(client_, image_uri, image_dir, blobs_root_);
  }
}

boost::filesystem::path RestorableAppEngine::installAppAndImages(const App& app) {
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  const auto app_install_dir{install_root_ / app.name};
  LOG_DEBUG << app.name << ": installing App: " << app_dir << " --> " << app_install_dir;
  installApp(app_dir, app_install_dir);
  LOG_DEBUG << app.name << ": installing App images: " << app_dir << " --> docker://";
  installAppImages(app_dir);
  return app_install_dir;
}

void RestorableAppEngine::installApp(const boost::filesystem::path& app_dir, const boost::filesystem::path& dst_dir) {
  const Manifest manifest{Utils::parseJSONFile(app_dir / Manifest::Filename)};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  boost::filesystem::create_directories(dst_dir);
  auto cmd = boost::str(boost::format("tar --overwrite -xzf %s") % archive_full_path.string());
  if (0 != boost::process::system(cmd, boost::process::start_dir = dst_dir)) {
    throw std::runtime_error("failed to install Compose App: " + app_dir.string());
  }
}

void RestorableAppEngine::installAppImages(const boost::filesystem::path& app_dir) {
  const auto compose{ComposeInfo((app_dir / ComposeFile).string())};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);
    LOG_ERROR << image_uri;

    const Uri uri{Uri::parseUri(image_uri)};
    const std::string tag{uri.registryHostname + '/' + uri.repo + ':' + uri.digest.shortHash()};
    const auto image_dir{app_dir / "images" / uri.registryHostname / uri.repo / uri.digest.hash()};
    installImage(client_, image_dir, blobs_root_, docker_host_, tag);
  }
}

bool RestorableAppEngine::isAppInstalled(const App& app) const {
  bool res{false};
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  const auto app_install_dir{install_root_ / app.name};

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

    const Manifest manifest{Utils::parseJSONFile(manifest_file)};
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

    auto cmd = boost::str(boost::format("tar -xzf %s %s") % archive_full_path % ComposeFile);
    if (0 != boost::process::system(cmd, boost::this_process::environment(), boost::process::start_dir = app_dir)) {
      LOG_DEBUG << app.name
                << ": failed to extract a compose file from the compose app archive: " + archive_full_path.string();
      break;
    }

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
    // TODO: check if App images are installed, require a function to generate sha256 hash while reading data from a
    // file.
    res = true;
  } while (0);

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

  boost::format cmd_fmt{"%s copy -f %s --dest-shared-blob-dir %s docker://%s oci:%s"};
  const auto cmd{boost::str(cmd_fmt % client % format % shared_blob_dir.string() % uri % dst_dir.string())};

  if (0 != boost::process::system(cmd, boost::this_process::environment())) {
    throw std::runtime_error("failed to install image " + uri);
  }
}

void RestorableAppEngine::installImage(const std::string& client, const boost::filesystem::path& image_dir,
                                       const boost::filesystem::path& shared_blob_dir, const std::string& docker_host,
                                       const std::string& tag, const std::string& format) {
  boost::format cmd_fmt{"%s copy -f %s --dest-daemon-host %s --src-shared-blob-dir %s oci:%s docker-daemon:%s"};
  LOG_ERROR << image_dir;
  std::string cmd{
      boost::str(cmd_fmt % client % format % docker_host % shared_blob_dir.string() % image_dir.string() % tag)};
  if (0 != boost::process::system(cmd, boost::this_process::environment())) {
    throw std::runtime_error("failed to install image ");
  }
}

void RestorableAppEngine::startComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir,
                                          const std::string& flags) {
  boost::format cmd_fmt{"%s up %s"};
  const auto cmd{boost::str(cmd_fmt % compose_cmd % flags)};
  if (0 != boost::process::system(cmd, boost::process::start_dir = app_dir)) {
    throw std::runtime_error("failed to bring Compose App up: " + cmd);
  }
}

}  // namespace Docker
