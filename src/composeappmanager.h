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

  bool fetchTarget(const Uptane::Target &target, Uptane::Fetcher &fetcher, const KeyManager &keys,
                   FetcherProgressCb progress_cb, const api::FlowControlToken *token) override;
  data::InstallationResult install(const Uptane::Target &target) const override;
  std::string name() const override { return PACKAGE_MANAGER_COMPOSEAPP; };

 private:
  FRIEND_TEST(ComposeApp, getApps);
  FRIEND_TEST(ComposeApp, handleRemovedApps);

  std::vector<std::pair<std::string, std::string>> getApps(const Uptane::Target &t) const;
  void handleRemovedApps(const Uptane::Target &target) const;

  ComposeAppConfig cfg_;
};

#endif  // COMPOSE_BUNDLES_H_
