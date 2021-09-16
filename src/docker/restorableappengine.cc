#include "restorableappengine.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "docker/composeinfo.h"

namespace Docker {

const std::string RestorableAppEngine::ComposeFile{"docker-compose.yml"};

RestorableAppEngine::RestorableAppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
                                         Docker::RegistryClient::Ptr registry_client, const std::string& client,
                                         const std::string& docker_host)
    : store_root_{std::move(store_root)},
      install_root_{std::move(install_root)},
      client_{std::move(client)},
      docker_host_{std::move(docker_host)},
      registry_client_{std::move(registry_client)} {
  boost::filesystem::create_directories(apps_root_);
  boost::filesystem::create_directories(blobs_root_);
}

bool RestorableAppEngine::fetch(const App& app) {
  bool res{true};
  try {
    const Uri uri{Uri::parseUri(app.uri)};
    const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
    LOG_DEBUG << app.name << ": downloading App from Registry: " << app.uri << " --> " << app_dir;
    const auto app_compose_file{pullApp(uri, app_dir)};

    const auto images_dir{app_dir / "images"};
    LOG_DEBUG << app.name << ": downloading App images from Registry(ies): " << app.uri << " --> " << images_dir;
    pullAppImages(app_compose_file, images_dir);
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to fetch App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
  }

  return res;
}

bool RestorableAppEngine::install(const App& app) {
  bool res{true};
  try {
    const Uri uri{Uri::parseUri(app.uri)};
    const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
    const auto app_install_dir{install_root_ / app.name};
    LOG_DEBUG << app.name << ": installing App: " << app_dir << " --> " << app_install_dir;
    installApp(app_dir, app_install_dir);
    LOG_DEBUG << app.name << ": installing App images: " << app_dir << " --> docker://";
    installAppImages(app_dir);
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to fetch App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
  }
  return res;
}

bool RestorableAppEngine::run(const App& app) {
  bool res{true};
  return res;
}

void RestorableAppEngine::remove(const App& app) {}

bool RestorableAppEngine::isRunning(const App& app) const {
  bool res{true};
  return res;
}

Json::Value RestorableAppEngine::getRunningAppsInfo() const { return Json::Value(); }

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

// static methods to manage image data

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

}  // namespace Docker
