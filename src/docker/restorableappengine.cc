#include "restorableappengine.h"
#include "docker/composeinfo.h"

#include <boost/process.hpp>

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
  // return pullImagesWithDocker(app);
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

bool cmd_str(const std::string& cmd) {
  LOG_DEBUG << "Running: " << cmd;
  int exit_code = boost::process::system(cmd);
  return exit_code == 0;
}

bool skopeo_copy(const std::string& auth, const std::string& src_uri, const std::string& dst_blob_path,
                 const std::string& dst_path) {
  std::string cmd{"skopeo copy -f v2s2 --src-creds=" + auth + " --dest-shared-blob-dir " + dst_blob_path +
                  " docker://" + src_uri + " oci:" + dst_path};
  LOG_ERROR << "???? " << cmd;
  return cmd_str(cmd);
}

bool RestorableAppEngine::pullImagesWithSkopeo(const App& app) {
  std::string fio_hub_auth;
  use_restore_root_ = true;
  ComposeInfo compose{(appRoot(app) / ComposeAppEngine::ComposeFile).string()};
  for (const auto& service : compose.getServices()) {
    const auto image = compose.getImage(service);
    LOG_ERROR << ">>>" << image;
    const Uri uri{Uri::parseUri(image)};
    std::string auth{"anonymous:"};

    if (uri.registryHostname == "hub.foundries.io") {
      if (fio_hub_auth.empty()) {
        fio_hub_auth = registryClient()->getBasicAuthMaterial();
      }
      auth = fio_hub_auth;
    }
    const auto dst_path{images_root_ / uri.registryHostname / uri.repo};
    boost::filesystem::create_directories(dst_path);
    skopeo_copy(auth, image, images_blobs_root_.string(), dst_path.string());
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
    std::string cmd{"skopeo copy --src-shared-blob-dir " + images_blobs_root_.string() + " oci:" + src_path.string() +
                    " docker-daemon:" + uri.registryHostname + '/' + uri.repo + ":latest"};
    LOG_ERROR << "???? " << cmd;
    cmd_str(cmd);
  }
  return true;
}

}  // namespace Docker
