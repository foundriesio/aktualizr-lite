#ifndef AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
#define AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_

#include "bootloader/bootloaderlite.h"
#include "ostree/sysroot.h"
#include "package_manager/ostreemanager.h"

class RootfsTreeManager : public OstreeManager {
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
                    std::shared_ptr<OSTree::Sysroot> sysroot)
      : OstreeManager(pconfig, bconfig, storage, http, new BootloaderLite(bconfig, *storage)),
        sysroot_{std::move(sysroot)},
        http_client_{http},
        gateway_url_{pconfig.ostree_server} {}

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

 private:
  std::string getCurrentHash() const override { return sysroot_->getCurDeploymentHash(); }
  void getAdditionalRemotes(std::vector<Remote>& remotes, const std::string& target_name);

  void setRemote(const std::string& name, const std::string& url, const boost::optional<const KeyManager*>& keys);

  std::shared_ptr<OSTree::Sysroot> sysroot_;
  std::shared_ptr<HttpInterface> http_client_;
  const std::string gateway_url_;
};

#endif  // AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
