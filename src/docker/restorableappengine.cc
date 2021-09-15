#include "restorableappengine.h"

namespace Docker {

RestorableAppEngine::RestorableAppEngine() {}

bool RestorableAppEngine::fetch(const App& app) {
  bool res{true};
  return res;
}

bool RestorableAppEngine::install(const App& app) {
  bool res{true};
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

}  // namespace Docker
