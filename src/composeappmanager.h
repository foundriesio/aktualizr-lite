#ifndef COMPOSE_BUNDLES_H_
#define COMPOSE_BUNDLES_H_

#include "package_manager/ostreemanager.h"

#define PACKAGE_MANAGER_COMPOSEAPP "ostree+compose_apps"

class ComposeAppManager : public OstreeManager {
 public:
  ComposeAppManager(const PackageConfig &pconfig, const BootloaderConfig &bconfig,
                    const std::shared_ptr<INvStorage> &storage, const std::shared_ptr<HttpInterface> &http)
      : OstreeManager(pconfig, bconfig, storage, http) {}

  std::string name() const override { return PACKAGE_MANAGER_COMPOSEAPP; };
};

#endif  // COMPOSE_BUNDLES_H_
