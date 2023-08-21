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

    // A high watermark for storage usage, expressed as a percentage,
    // in other words, up to X% of the overall volume capacity can be used.
    // The volume on which the sysroot is persisted is what is meant in this context.
    unsigned int StorageWatermark{DefaultStorageWatermark};
  };

  enum class Deployment { kCurrent = 0, kPending, kRollback };
  using Ptr = std::shared_ptr<Sysroot>;

  explicit Sysroot(const PackageConfig& pconfig, std::string sysroot_path, BootedType booted = BootedType::kBooted,
                   std::string os_name = "lmp");

  const std::string& path() const { return path_; }
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
  const std::string path_;
  const std::string repo_path_;
  const BootedType booted_;
  const std::string os_name_;
  const std::string deployment_path_;

  GObjectUniquePtr<OstreeSysroot> sysroot_;
};

}  // namespace OSTree

#endif  // AKTUALIZR_LITE_OSTREE_H_
