#include "restorableappengine.h"
#include "docker/composeinfo.h"

#include <boost/process.hpp>
#include <boost/format.hpp>


namespace Docker {

bool RestorableAppEngine::download(const App& app) {
  use_restore_root_ = true;

  boost::filesystem::create_directories(appRoot(app));
  boost::filesystem::create_directories(images_root_);
  auto res = ComposeAppEngine::download(app);

  use_restore_root_ = false;
  return res;
}

bool RestorableAppEngine::verify(const App& app) {
  use_restore_root_ = true;
  auto res = ComposeAppEngine::verify(app);
  use_restore_root_ = false;
  return res;
}

bool RestorableAppEngine::pullImages(const App& app) {
   //return pullImagesWithDocker(app);
  return pullImagesWithSkopeo(app);
}

bool RestorableAppEngine::installApp(const App& app) {
  // installAppWithDocker(app);
  installAppWithSkopeo(app);
  return ComposeAppEngine::installApp(app);
}

bool RestorableAppEngine::start(const App& app) {
  // installAppWithDocker(app);
  installAppWithSkopeo(app);

  return ComposeAppEngine::start(app);
}

boost::filesystem::path RestorableAppEngine::appRoot(const App& app) const {
  if (use_restore_root_) {
    return apps_root_ / app.name;
  } else {
    return ComposeAppEngine::appRoot(app);
  }
}

bool RestorableAppEngine::pullImagesWithDocker(const App& app) {
  use_restore_root_ = true;

  if (!ComposeAppEngine::pullImages(app)) {
    use_restore_root_ = false;
    return false;
  }

  ComposeInfo compose{(appRoot(app) / ComposeAppEngine::ComposeFile).string()};
  for (const auto& service : compose.getServices()) {
    const auto image = compose.getImage(service);
    const auto service_hash = compose.getHash(service);
    std::string cmd{"docker save " + image + " -o " + service_hash + ".tar"};
    LOG_DEBUG << "Running: " << cmd;
    int exit_code = boost::process::system(cmd, boost::process::start_dir = images_root_);
    if (exit_code != 0) {
      use_restore_root_ = false;
      return false;
    }
  }
  use_restore_root_ = false;
  return true;
}

bool RestorableAppEngine::installAppWithDocker(const App& app) {
  Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  const std::string archive_file_name{uri.digest.shortHash() + '.' + app.name + ArchiveExt};
  extractAppArchive(app, (apps_root_ / app.name / archive_file_name).string());
  return true;
}

bool RestorableAppEngine::pullImagesWithSkopeo(const App& app) {

  use_restore_root_ = true;
  std::string fio_hub_auth;

  ComposeInfo compose{(appRoot(app) / ComposeAppEngine::ComposeFile).string()};
  for (const auto& service : compose.getServices()) {
    const auto image = compose.getImage(service);
    const Uri uri{Uri::parseUri(image)};
    std::string auth;

    if (uri.registryHostname == "hub.foundries.io") {
      if (fio_hub_auth.empty()) {
        fio_hub_auth = registryClient()->getBasicAuthMaterial();
      }
      auth = fio_hub_auth;
    }
    const auto dst_path{images_root_ / uri.registryHostname / uri.repo};
    boost::filesystem::create_directories(dst_path);

    skopeo_cmd_.pullFromRegistry(image, dst_path.string(), images_blobs_root_.string(), auth);
  }

  use_restore_root_ = false;
  return true;
}

bool RestorableAppEngine::installAppWithSkopeo(const App& app) {
  // extract a compose app archive to compose-apps/<app> dir
  installAppWithDocker(app);

  // copy images from <reset-apps-dir>/images to the docker daemon dir
  ComposeInfo compose{(appRoot(app) / ComposeAppEngine::ComposeFile).string()};
  for (const auto& service : compose.getServices()) {
    const auto image = compose.getImage(service);
    const Uri uri{Uri::parseUri(image)};

    const auto src_path{images_root_ / uri.registryHostname / uri.repo};
    skopeo_cmd_.copyToDockerStore(src_path.string(), images_blobs_root_.string(), uri.registryHostname + '/' + uri.repo + ':' + uri.digest.shortHash());
  }
  return true;
}

bool SkopeoCmd::run_cmd(const std::string& cmd) {
  LOG_DEBUG << "Running: " << cmd;
  int exit_code = boost::process::system(cmd, boost::this_process::environment());
  return exit_code == 0;
}

bool SkopeoCmd::pullFromRegistry(const std::string& srs, const std::string& dst_images, const std::string& dst_blobs, const std::string& auth) {
  std::string cmd;
  const std::string format{ManifestFormat};

  if (auth.empty()) {
    // use REGISTRY_AUTH_FILE env var for the aktualizr service to setup
    // access to private Docker Registries, e.g.
    // export REGISTRY_AUTH_FILE=/usr/lib/docker/config.json
    // The file lists docker cred helpers
    boost::format cmd_fmt{"%s copy -f %s --dest-shared-blob-dir %s docker://%s oci:%s"};
    cmd = boost::str(cmd_fmt % skopeo_bin_ % format % dst_blobs % srs % dst_images);
  } else {
    boost::format cmd_fmt{"%s copy -f %s --src-creds %s --dest-shared-blob-dir %s docker://%s oci:%s"};
    cmd = boost::str(cmd_fmt % skopeo_bin_ % format % auth % dst_blobs % srs % dst_images);
  }

  return run_cmd(cmd);
}

bool SkopeoCmd::copyToDockerStore(const std::string& srs_image, const std::string& src_blobs, const std::string& dst) {
  const std::string format{ManifestFormat};
  boost::format cmd_fmt{"%s copy -f %s --src-shared-blob-dir %s oci:%s docker-daemon:%s"};
  std::string cmd{boost::str(cmd_fmt % skopeo_bin_ % format % src_blobs % srs_image % dst)};
  return run_cmd(cmd);
}

}  // namespace Docker
