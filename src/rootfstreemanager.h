#ifndef AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
#define AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_

#include "bootloader/bootloaderlite.h"
#include "package_manager/ostreemanager.h"

class RootfsTreeManager : public OstreeManager {
 public:
  static constexpr const char *const Name{"ostree"};

  RootfsTreeManager(const PackageConfig &pconfig, const BootloaderConfig &bconfig,
                    const std::shared_ptr<INvStorage> &storage, const std::shared_ptr<HttpInterface> &http,
                    std::shared_ptr<OSTree::Sysroot> sysroot)
      : OstreeManager(pconfig, bconfig, storage, http, new BootloaderLite(bconfig, *storage)),
        sysroot_{std::move(sysroot)} {}

 private:
  std::string getCurrentHash() const { return sysroot_->getCurDeploymentHash(); }

 private:
  std::shared_ptr<OSTree::Sysroot> sysroot_;
};

#endif  // AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
