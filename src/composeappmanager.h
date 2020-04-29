#ifndef COMPOSE_BUNDLES_H_
#define COMPOSE_BUNDLES_H_

#include "package_manager/ostreemanager.h"

#define PACKAGE_MANAGER_COMPOSEAPP "ostree+compose_apps"

class ComposeAppConfig {
 public:
  ComposeAppConfig(const PackageConfig &pconfig);

  std::vector<std::string> apps;
  boost::filesystem::path apps_root;
  boost::filesystem::path compose_bin{"/usr/bin/docker-compose"};
  bool docker_prune{true};
};

class ComposeAppManager : public OstreeManager {
 public:
  ComposeAppManager(const PackageConfig &pconfig, const BootloaderConfig &bconfig,
                    const std::shared_ptr<INvStorage> &storage, const std::shared_ptr<HttpInterface> &http)
      : OstreeManager(pconfig, bconfig, storage, http), cfg_(pconfig) {}

  std::string name() const override { return PACKAGE_MANAGER_COMPOSEAPP; };

 private:
  ComposeAppConfig cfg_;
};

#endif  // COMPOSE_BUNDLES_H_
