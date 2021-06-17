#ifndef AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
#define AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_

#include <functional>
#include <memory>
#include <unordered_map>

#include "docker/composeappengine.h"
#include "docker/composeapptree.h"
#include "docker/docker.h"
#include "ostree/sysroot.h"
#include "package_manager/ostreemanager.h"

class ComposeAppManager : public OstreeManager {
 public:
  static constexpr const char* const Name{"ostree+compose_apps"};

  struct Config {
   public:
    Config(const PackageConfig& pconfig);

    boost::optional<std::vector<std::string>> apps;
    boost::filesystem::path apps_root{"/var/sota/compose-apps"};
    boost::filesystem::path compose_bin{"/usr/bin/docker-compose"};
    bool docker_prune{true};
    bool force_update{false};
    boost::filesystem::path apps_tree{"/var/sota/compose-apps-tree"};
    bool create_apps_tree{false};
    boost::filesystem::path images_data_root{"/var/lib/docker"};
    std::string docker_images_reload_cmd{"systemctl reload docker"};
    std::string hub_auth_creds_endpoint{Docker::RegistryClient::DefAuthCredsEndpoint};
  };

  using AppsContainer = std::unordered_map<std::string, std::string>;

  ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot, AppEngine::Ptr app_engine = nullptr);

  std::string name() const override { return Name; }
  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

  data::InstallationResult install(const Uptane::Target& target) const override;
  data::InstallationResult finalizeInstall(const Uptane::Target& target) override;

  // Returns an intersection of Target's Apps and Apps listed in the config (sota.toml:compose_apps)
  // If Apps are not specified in the config then all Target's Apps are returned
  AppsContainer getApps(const Uptane::Target& t) const;
  AppsContainer getAppsToUpdate(const Uptane::Target& t) const;
  void setApps(std::vector<std::string>& apps) { *cfg_.apps = apps; }
  bool checkForAppsToUpdate(const Uptane::Target& target);
  void setAppsNotChecked() { are_apps_checked_ = false; }
  void handleRemovedApps(const Uptane::Target& target) const;

 private:
  std::string getCurrentHash() const override;
  Json::Value getRunningAppsInfo() const;
  std::string getRunningAppsInfoForReport() const;

 private:
  Config cfg_;
  std::shared_ptr<OSTree::Sysroot> sysroot_;
  mutable AppsContainer cur_apps_to_fetch_and_update_;
  bool are_apps_checked_{false};
  AppEngine::Ptr app_engine_;
  std::unique_ptr<ComposeAppTree> app_tree_;
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
