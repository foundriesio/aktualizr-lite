#include "restorableappengine.h"

namespace Docker {

RestorableAppEngine::RestorableAppEngine() {}

bool RestorableAppEngine::fetch(const App& app) {}

bool RestorableAppEngine::install(const App& app) {}

bool RestorableAppEngine::run(const App& app) {}

void RestorableAppEngine::remove(const App& app) {}

bool RestorableAppEngine::isRunning(const App& app) const {}

Json::Value RestorableAppEngine::getRunningAppsInfo() const {}

}  // namespace Docker
