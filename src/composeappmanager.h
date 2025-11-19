#ifndef AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
#define AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_

#include <functional>
#include <memory>
#include <set>
#include <unordered_map>

#include "docker/composeappengine.h"
#include "docker/docker.h"
#include "ostree/sysroot.h"
#include "rootfstreemanager.h"

class ComposeAppManager : public RootfsTreeManager {
 public:
  static constexpr const char* const Name{"ostree+compose_apps"};

  struct Config {
   public:
    explicit Config(const PackageConfig& pconfig);

    boost::optional<std::vector<std::string>> apps;
    boost::optional<std::vector<std::string>> reset_apps;
    boost::filesystem::path apps_root{"/var/sota/compose-apps"};
    boost::filesystem::path reset_apps_root{"/var/sota/reset-apps"};
    boost::filesystem::path compose_bin{"/usr/bin/docker"};
    boost::filesystem::path skopeo_bin{"/sbin/skopeo"};
#ifdef USE_COMPOSEAPP_ENGINE
    boost::filesystem::path composectl_bin{"/usr/bin/composectl"};
    std::string apps_proxy;
    std::string apps_proxy_ca;
#endif  // USE_COMPOSEAPP_ENGINE
    bool docker_prune{true};
    bool force_update{false};
    boost::filesystem::path apps_tree{"/var/sota/compose-apps-tree"};
    bool create_apps_tree{false};
    boost::filesystem::path images_data_root{"/var/lib/docker"};
    std::string docker_images_reload_cmd{"systemctl reload docker"};
    std::string hub_auth_creds_endpoint{Docker::RegistryClient::DefAuthCredsEndpoint};
    bool create_containers_before_reboot{true};
    bool stop_apps_before_update{true};
    int storage_watermark{80};
  };

  using AppsContainer = std::unordered_map<std::string, std::string>;
  using AppsSyncReason = std::unordered_map<std::string, std::string>;

  ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot, const KeyManager& keys,
                    AppEngine::Ptr app_engine = nullptr);

  std::string name() const override { return Name; }
  DownloadResult Download(const TufTarget& target) override;
  data::InstallationResult Install(const TufTarget& target, InstallMode mode) override;
  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

  TargetStatus verifyTarget(const Uptane::Target& target) const override;
  data::InstallationResult install(const Uptane::Target& target) const override;
  data::InstallationResult finalizeInstall(const Uptane::Target& target) override;

  // Returns an intersection of Target's Apps and Apps listed in the config (sota.toml:compose_apps)
  // If Apps are not specified in the config then all Target's Apps are returned
  AppsContainer getApps(const Uptane::Target& t) const;
  AppsContainer getAppsToUpdate(const Uptane::Target& t, AppsSyncReason& apps_and_reasons,
                                std::set<std::string>& fetched_apps) const;
  bool isAppRunning(const AppEngine::App& app);
  AppsSyncReason checkForAppsToUpdate(const Uptane::Target& target);
  void setAppsNotChecked() { are_apps_checked_ = false; }
  void handleRemovedApps(const Uptane::Target& target) const;
  Json::Value getAppsState() const;
  static bool compareAppsStates(const Json::Value& left, const Json::Value& right);
  static AppsContainer getRequiredApps(const Config& cfg, const Uptane::Target& target);

 private:
  void completeInitialTarget(Uptane::Target& init_target) override;
  Json::Value getRunningAppsInfo() const;
  std::string getRunningAppsInfoForReport() const;

  AppsContainer getAppsToFetch(const Uptane::Target& target, bool check_store = true,
                               const AppsContainer* checked_apps = nullptr,
                               const std::set<std::string>* fetched_apps = nullptr) const;
  void stopDisabledComposeApps(const Uptane::Target& target) const;
  void removeDisabledComposeApps(const Uptane::Target& target) const;
  void forEachRemovedApp(const Uptane::Target& target,
                         const std::function<void(AppEngine::Ptr&, const std::string&)>& action) const;
  std::string getAppsFsUsageInfo() const;

  Config cfg_;
  mutable AppsContainer cur_apps_to_fetch_and_update_;
  mutable AppsContainer cur_apps_to_fetch_;
  bool are_apps_checked_{false};
  AppEngine::Ptr app_engine_;
  bool is_restorable_engine_{false};
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
