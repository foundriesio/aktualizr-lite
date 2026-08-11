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
    // Percentage (20-95) of overall storage that Apps may use. Forwarded to composectl as
    // --storage-usage-watermark unless reserved_storage takes over.
    int storage_watermark{80};
    // reserved_storage, when set, reserves an absolute amount of free space (e.g. "2GiB" or "500MB")
    // instead of a percentage and takes precedence over storage_watermark. reserved_storage holds
    // the raw config value forwarded to composectl as --reserved-storage; reserved_storage_bytes is
    // its parsed size in bytes (0 when reserved_storage is unset).
    std::string reserved_storage{};
    uint64_t reserved_storage_bytes{0};

    // Returns the (limit, in-bytes) pair for the local storage-space check: the reserved free
    // space in bytes when reserved_storage is set, otherwise the percentage watermark.
    std::pair<uint64_t, bool> storageSpaceLimit() const {
      if (reserved_storage_bytes > 0) {
        return {reserved_storage_bytes, true};
      }
      return {static_cast<uint64_t>(storage_watermark), false};
    }

    // Parses a human-readable byte size (e.g. "2GiB", "500MiB", "2GB", "500MB") into a count of
    // bytes. The presence of an "i" in the suffix selects the binary base (1024); otherwise the
    // decimal base (1000) is used. Returns false when `value` lacks a recognized size unit, when
    // the numeric literal overflows double, or when the scaled byte count would overflow uint64_t.
    static bool parseSizeInBytes(const std::string& value, uint64_t& bytes);
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
  bool areAppsChecked() const { return are_apps_checked_; }
  void setAppsCheckFlag(bool are_apps_checked) {
    LOG_DEBUG << "Setting the apps check flag to " << are_apps_checked;
    are_apps_checked_ = are_apps_checked;
  }
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
  // Checks, before any download starts, that the combined ostree + apps update
  // fits, accounting for the ostree repo, app/blob store and docker store
  // possibly living on different volumes. Returns a DownloadFailed_NoSpace
  // result when a volume is short; an Ok result otherwise (including when sizes
  // cannot be estimated, in which case the per-pull checks remain the backstop).
  DownloadResult checkUpdateSize(const TufTarget& target, const AppsContainer& apps_to_fetch);

  Config cfg_;
  mutable AppsContainer cur_apps_to_fetch_and_update_;
  mutable AppsContainer cur_apps_to_fetch_;
  bool are_apps_checked_{false};
  AppEngine::Ptr app_engine_;
  bool is_restorable_engine_{false};
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
