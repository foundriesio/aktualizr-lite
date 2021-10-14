#ifndef AKTUALIZR_LITE_OSTREE_H_
#define AKTUALIZR_LITE_OSTREE_H_

#include <ostree.h>
#include <string>

#include "package_manager/ostreemanager.h"

namespace OSTree {

class Sysroot {
 public:
  explicit Sysroot(std::string sysroot_path, BootedType booted = BootedType::kBooted);

  const std::string& path() const { return path_; }

  virtual std::string getCurDeploymentHash() const;

 private:
  const std::string path_;
  const BootedType booted_;

  GObjectUniquePtr<OstreeSysroot> sysroot_;
};

}  // namespace OSTree

#endif  // AKTUALIZR_LITE_OSTREE_H_
