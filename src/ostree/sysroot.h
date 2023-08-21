#ifndef AKTUALIZR_LITE_OSTREE_H_
#define AKTUALIZR_LITE_OSTREE_H_

#include <ostree.h>
#include <string>

#include "package_manager/ostreemanager.h"

namespace OSTree {

class Sysroot {
 public:
  struct Config {
   public:
    explicit Config(const PackageConfig& pconfig);

    static constexpr const char* const StorageWatermarkParamName{"sysroot_storage_watermark"};
    static const unsigned int DefaultStorageWatermark;
    static const unsigned int MinStorageWatermark;
    static const unsigned int MaxStorageWatermark;

    static constexpr const char* const StorageFreeSpacePercentParamName{"min_free_space_percent"};
    static const unsigned int MinFreeSpacePercent;
    static const unsigned int MaxFreeSpacePercent;

    // A high watermark for storage usage, expressed as a percentage,
    // in other words, up to X% of the overall volume capacity can be used.
    // The volume on which the sysroot is persisted is what is meant in this context.
    unsigned int StorageWatermark{DefaultStorageWatermark};

    // A low watermark for storage free space expressed as a percentage.
    // In other words, at least X% of the overall volume capacity must be free, or
    // up to (100 - X%) of the overall volume capacity can be used.
    // This parameter is intended solely for the non-delta case.
    // Effectively, it enforces setting of the ostree repo config param `core.min-free-space-percent`.
    int StorageFreeSpacePercent{-1};

    std::string Path;
    BootedType Type;
    std::string OsName;
  };

  enum class Deployment { kCurrent = 0, kPending, kRollback };
  using Ptr = std::shared_ptr<Sysroot>;

  explicit Sysroot(const PackageConfig& pconfig);

  const std::string& path() const { return cfg_.Path; }
  const std::string& repoPath() const { return repo_path_; }
  const std::string& deployment_path() const { return deployment_path_; }

  virtual std::string getDeploymentHash(Deployment deployment_type) const;
  bool reload();
  unsigned int storageWatermark() const { return cfg_.StorageWatermark; }

 private:
  static OstreeDeployment* getDeploymentIfBooted(OstreeSysroot* sysroot, const char* os_name,
                                                 Deployment deployment_type);
  static OstreeDeployment* getDeploymentIfStaged(OstreeSysroot* sysroot, const char* os_name,
                                                 Deployment deployment_type);

  const Config cfg_;
  const std::string repo_path_;
  const std::string deployment_path_;

  GObjectUniquePtr<OstreeSysroot> sysroot_;
};

}  // namespace OSTree

#endif  // AKTUALIZR_LITE_OSTREE_H_
