#ifndef AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
#define AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_

#include <functional>
#include <memory>
#include <unordered_map>

#include "composeapp.h"
#include "composeapptree.h"
#include "docker.h"
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
    boost::filesystem::path docker_bin{"/usr/bin/docker"};
    bool docker_prune{true};
    bool force_update{false};
    boost::filesystem::path apps_tree{"/var/sota/compose-apps-tree"};
    bool create_apps_tree{false};
    boost::filesystem::path images_data_root{"/var/lib/docker"};
    std::string docker_images_reload_cmd{"systemctl reload docker"};
  };

  using ComposeAppCtor = std::function<Docker::ComposeApp(const std::string& app)>;
  using AppsContainer = std::unordered_map<std::string, std::string>;

  ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot,
                    Docker::RegistryClient::HttpClientFactory registry_http_client_factory =
                        Docker::RegistryClient::DefaultHttpClientFactory);

  std::string name() const override { return Name; }
  Uptane::Target getCurrent() const override;
  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

  data::InstallationResult install(const Uptane::Target& target) const override;
  data::InstallationResult finalizeInstall(const Uptane::Target& target) override;
  void handleRemovedApps(const Uptane::Target& target) const;

  boost::optional<std::vector<std::string>>& getAppShortlist() { return cfg_.apps; }

 private:
  std::string getCurrentHash() const override;
  // Return a description of what `docker ps` sees
  std::string containerDetails() const;

 private:
  Config cfg_;
  std::shared_ptr<OSTree::Sysroot> sysroot_;
  Docker::RegistryClient registry_client_;
  // mutable AppsContainer cur_apps_to_fetch_and_update_;
  bool are_apps_checked_{false};
  ComposeAppCtor app_ctor_;
  std::unique_ptr<ComposeAppTree> app_tree_;
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
