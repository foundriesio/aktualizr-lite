#include "restorableappengine.h"
#include "docker/composeinfo.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

namespace Docker {

bool RestorableAppEngine::fetch(const App& app) { return download(app) && verify(app) && pullImages(app); }

bool RestorableAppEngine::download(const App& app) {
  bool res{true};

  try {
    app_store_->pullApp(app);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to download App: " << exc.what();
    res = false;
  }
  return res;
}

bool RestorableAppEngine::verify(const App& app) {
  use_restore_root_ = true;
  auto res = ComposeAppEngine::verify(app);
  use_restore_root_ = false;
  return res;
}

bool RestorableAppEngine::pullImages(const App& app) {
  bool res = false;
  use_restore_root_ = true;
  std::string fio_hub_auth;

  ComposeInfo compose{(appRoot(app) / ComposeAppEngine::ComposeFile).string()};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);
    const Uri uri{Uri::parseUri(image_uri)};
    std::string auth;

    if (uri.registryHostname == "hub.foundries.io") {
      if (fio_hub_auth.empty()) {
        fio_hub_auth = registryClient()->getBasicAuthMaterial();
      }
      auth = fio_hub_auth;
    }

    res = app_store_->pullAppImage(app, image_uri, auth);
  }

  use_restore_root_ = false;
  return res;
}

bool RestorableAppEngine::install(const App& app) {
  // extract a compose app archive to compose-apps/<app> dir
  boost::filesystem::create_directories(ComposeAppEngine::appRoot(app) / MetaDir);
  AppState state(app, ComposeAppEngine::appRoot(app), true);
  app_store_->copyApp(app, ComposeAppEngine::appRoot(app));
  state.setState(AppState::State::kPulled);

  return ComposeAppEngine::install(app);
}

bool RestorableAppEngine::run(const App& app) {
  // extract a compose app archive to compose-apps/<app> dir
  boost::filesystem::create_directories(ComposeAppEngine::appRoot(app) / MetaDir);
  AppState state(app, ComposeAppEngine::appRoot(app), true);
  app_store_->copyApp(app, ComposeAppEngine::appRoot(app));
  state.setState(AppState::State::kPulled);

  return ComposeAppEngine::run(app);
}

bool RestorableAppEngine::installApp(const App& app) {
  return installAppImages(app) && ComposeAppEngine::installApp(app);
}

bool RestorableAppEngine::start(const App& app) { return installAppImages(app) && ComposeAppEngine::start(app); }

boost::filesystem::path RestorableAppEngine::appRoot(const App& app) const {
  if (use_restore_root_) {
    return app_store_->appRoot(app);
  } else {
    return ComposeAppEngine::appRoot(app);
  }
}

bool RestorableAppEngine::installAppImages(const App& app) {
  bool res{false};

  // copy images from <reset-apps-dir>/images to the docker daemon dir
  ComposeInfo compose{(appRoot(app) / ComposeAppEngine::ComposeFile).string()};
  for (const auto& service : compose.getServices()) {
    const auto image = compose.getImage(service);
    res = app_store_->copyAppImageToDockerStore(app, image);
  }
  return res;
}

void RestorableAppEngine::purge(const Apps& app_shortlist) const { app_store_->purge(app_shortlist); }

}  // namespace Docker
