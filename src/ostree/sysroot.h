#ifndef AKTUALIZR_LITE_OSTREE_H_
#define AKTUALIZR_LITE_OSTREE_H_

#include <ostree.h>
#include <string>

#include "package_manager/ostreemanager.h"

namespace OSTree {

class Sysroot {
 public:
  enum class Deployment { kCurrent = 0, kPending, kRollback };

  explicit Sysroot(std::string sysroot_path, BootedType booted = BootedType::kBooted, std::string os_name = "lmp");

  const std::string& path() const { return path_; }
  const std::string& deployment_path() const { return deployment_path_; }

  virtual std::string getDeploymentHash(Deployment deployment_type) const;
  bool reload();

 private:
  static OstreeDeployment* getDeploymentIfBooted(OstreeSysroot* sysroot, const char* os_name,
                                                 Deployment deployment_type);
  static OstreeDeployment* getDeploymentIfStaged(OstreeSysroot* sysroot, const char* os_name,
                                                 Deployment deployment_type);

  const std::string path_;
  const BootedType booted_;
  const std::string os_name_;
  const std::string deployment_path_;

  GObjectUniquePtr<OstreeSysroot> sysroot_;
};

}  // namespace OSTree

#endif  // AKTUALIZR_LITE_OSTREE_H_
