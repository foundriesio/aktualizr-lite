#ifndef AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
#define AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_

#include "bootloader/bootloaderlite.h"
#include "downloader.h"
#include "http/httpinterface.h"
#include "ostree/sysroot.h"
#include "package_manager/ostreemanager.h"

class RootfsTreeManager : public OstreeManager, public Downloader {
 public:
  static constexpr const char* const Name{"ostree"};
  using RequestHeaders = std::unordered_map<std::string, std::string>;
  struct Remote {
    std::string name;
    std::string baseUrl;
    RequestHeaders headers;
    boost::optional<const KeyManager*> keys;
    bool isRemoteSet{false};
  };

  RootfsTreeManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot, const KeyManager& keys);

  DownloadResult Download(const TufTarget& target) override;

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

  const bootloader::BootFwUpdateStatus& bootFwUpdateStatus() const { return *boot_fw_update_status_; }
  void setInitialTargetIfNeeded(const std::string& hw_id);

 protected:
  virtual void completeInitialTarget(Uptane::Target& init_target){};
  void installNotify(const Uptane::Target& target) override;
  data::InstallationResult install(const Uptane::Target& target) const override;
  const std::shared_ptr<OSTree::Sysroot>& sysroot() const { return sysroot_; }

 private:
  struct DeltaStatsRef {
    std::string sha256;
    unsigned int size;
  };
  struct DeltaStat {
    uint64_t size;
    uint64_t uncompressedSize;
  };
  std::string getCurrentHash() const override {
    return sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent);
  }
  void getAdditionalRemotes(std::vector<Remote>& remotes, const std::string& target_name);

  void setRemote(const std::string& name, const std::string& url, const boost::optional<const KeyManager*>& keys);
  data::InstallationResult verifyBootloaderUpdate(const Uptane::Target& target) const;
  bool canUpdateFitOnDisk(const TufTarget& target, const Remote& remote) const;
  bool canDeltaFitOnDisk(const DeltaStat& delta_stat) const;

  static bool getDeltaStatsRef(const Json::Value& json, DeltaStatsRef& ref);
  static Json::Value downloadDeltaStats(const DeltaStatsRef& ref, const Remote& remote);
  static bool findDeltaStatForUpdate(const Json::Value& delta_stats, const std::string& from, const std::string& to,
                                     DeltaStat& found_delta_stat);
  static uint64_t getFreeSpace(const std::string& path);

  const KeyManager& keys_;
  std::shared_ptr<OSTree::Sysroot> sysroot_;
  std::unique_ptr<bootloader::BootFwUpdateStatus> boot_fw_update_status_;
  std::shared_ptr<HttpInterface> http_client_;
  const std::string gateway_url_;
  // A flag enabling/disabling ostree update blocking if there is ongoing boot firmware update
  // that requires confirmation by means of reboot.
  bool update_block_{true};
};

#endif  // AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
