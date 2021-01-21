#ifndef AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
#define AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_

#include <functional>
#include <unordered_map>

#include "composeapp.h"
#include "docker.h"
#include "ostree.h"
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
    bool full_status_check{false};
  };

  using ComposeAppCtor = std::function<Docker::ComposeApp(const std::string& app)>;
  using AppsContainer = std::unordered_map<std::string, std::string>;

  ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot,
                    Docker::RegistryClient::HttpClientFactory registry_http_client_factory =
                        Docker::RegistryClient::DefaultHttpClientFactory,
                    Docker::Engine::ClientFactory engine_client_factory = Docker::Engine::DefaultClientFactory);

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

  data::InstallationResult install(const Uptane::Target& target) const override;
  std::string name() const override { return Name; };

  // Returns an intersection of Target's Apps and Apps listed in the config (sota.toml:compose_apps)
  // If Apps are not specified in the config then all Target's Apps are returned
  AppsContainer getApps(const Uptane::Target& t) const;
  AppsContainer getAppsToUpdate(const Uptane::Target& t, bool full_status_check) const;
  bool checkForAppsToUpdate(const Uptane::Target& target, boost::optional<bool> full_status_check_in);
  void setAppsNotChecked() { are_apps_checked_ = false; }
  void handleRemovedApps(const Uptane::Target& target) const;
  std::string getCurrentHash() const override;

 private:
  Config cfg_;
  std::shared_ptr<OSTree::Sysroot> sysroot_;
  Docker::RegistryClient registry_client_;
  Docker::Engine engine_client_;
  mutable AppsContainer cur_apps_to_fetch_and_update_;
  bool are_apps_checked_{false};
  ComposeAppCtor app_ctor_;
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
