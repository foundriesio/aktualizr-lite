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

    static constexpr const char* const ReservedStorageSpacePercentageDeltaParamName{
        "sysroot_delta_reserved_space_percentage"};
    static const unsigned int DefaultReservedStorageSpacePercentageDelta;
    static const unsigned int MinReservedStorageSpacePercentageDelta;
    static const unsigned int MaxReservedStorageSpacePercentageDelta;

    static constexpr const char* const ReservedStorageSpacePercentageOstreeParamName{
        "sysroot_ostree_reserved_space_percentage"};
    static const unsigned int MinReservedStorageSpacePercentageOstree;
    static const unsigned int MaxReservedStorageSpacePercentageOstree;

    // This variable represents the reserved amount of storage, expressed as a percentage
    // of the overall capacity of the volume where the sysroot/ostree repo is located.
    // The reserved percentage is only considered when performing a delta-based ostree pull.
    // The downloader verifies that the reserved storage will remain untouched prior to initiating a delta-based ostree
    // pull. If the available free space, in addition to the reserved space, is insufficient to fit delta files, then
    // the downloader will reject the download and exit with an error.
    unsigned int ReservedStorageSpacePercentageDelta{DefaultReservedStorageSpacePercentageDelta};

    // This variable represents the reserved amount of storage, expressed as a percentage
    // of the overall capacity of the volume where the sysroot/ostree repo is located.
    // The reserved percentage is considered in the both cases, during performing
    // an object-based ostree pull and delta-based ostree pull.
    // The downloader guarantees that the reserved storage is untouched when ostree objects are being committed to an
    // ostree repo. If the available free space, in addition to the reserved space, is insufficient to fit object files,
    // then the downloader will reject the download and exit with an error.
    // Effectively, it enforces setting of the ostree repo config param `core.min-free-space-percent`.
    int ReservedStorageSpacePercentageOstree{-1};

    std::string path;
    BootedType type;
    std::string osname;
  };

  enum class Deployment { kCurrent = 0, kPending, kRollback };
  using Ptr = std::shared_ptr<Sysroot>;

  explicit Sysroot(const PackageConfig& pconfig);

  const std::string& path() const { return cfg_.path; }
  const std::string& repoPath() const { return repo_path_; }
  const std::string& deployment_path() const { return deployment_path_; }

  virtual std::string getDeploymentHash(Deployment deployment_type) const;
  bool reload();
  unsigned int reservedStorageSpacePercentageDelta() const { return cfg_.ReservedStorageSpacePercentageDelta; }
  unsigned int reservedStorageSpacePercentageOstree() const;

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
