#ifndef AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
#define AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_

#include "appengine.h"

namespace Docker {

class RestorableAppEngine : public AppEngine {
 public:
  RestorableAppEngine();

 public:
  bool fetch(const App& app) override;
  bool install(const App& app) override;
  bool run(const App& app) override;
  void remove(const App& app) override;
  bool isRunning(const App& app) const override;
  Json::Value getRunningAppsInfo() const override;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
