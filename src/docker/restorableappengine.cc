#include "restorableappengine.h"
#include "docker/composeinfo.h"

#include <boost/process.hpp>

namespace Docker {

bool RestorableAppEngine::download(const App& app) {
  use_restore_root_ = true;

  boost::filesystem::create_directories(appRoot(app));
  boost::filesystem::create_directories(root_ / "images");
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
    int exit_code = boost::process::system(cmd, boost::process::start_dir = root_ / "images");
    if (exit_code != 0) {
      use_restore_root_ = false;
      return false;
    }
  }
  use_restore_root_ = false;
  return true;
}

bool RestorableAppEngine::installApp(const App& app) {
  Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  const std::string archive_file_name{uri.digest.shortHash() + '.' + app.name + ArchiveExt};
  const auto restore_app_root = RestorableAppEngine::appRoot(app);
  extractAppArchive(app, (restore_app_root / archive_file_name).string());

  return ComposeAppEngine::installApp(app);
}

bool RestorableAppEngine::start(const App& app) {
  Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  const std::string archive_file_name{uri.digest.shortHash() + '.' + app.name + ArchiveExt};
  const auto restore_app_root = root_ / "apps" / app.name;
  extractAppArchive(app, (restore_app_root / archive_file_name).string());

  return ComposeAppEngine::start(app);
}

boost::filesystem::path RestorableAppEngine::appRoot(const App& app) const {
  if (use_restore_root_) {
    return root_ / "apps" / app.name;
  } else {
    return ComposeAppEngine::appRoot(app);
  }
}

}  // namespace Docker
