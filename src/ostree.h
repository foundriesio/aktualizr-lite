#ifndef AKTUALIZR_LITE_OSTREE_H_
#define AKTUALIZR_LITE_OSTREE_H_

#include <ostree.h>
#include <string>

#include "package_manager/ostreemanager.h"

namespace OSTree {

class Sysroot {
 public:
  Sysroot(const std::string& sysroot_path, bool booted = true);

  const std::string& path() const { return path_; }
  std::string type() const { return booted_ ? "booted" : "staged"; }

  virtual std::string getCurDeploymentHash() const;

 private:
  const std::string path_;
  const bool booted_;

  GObjectUniquePtr<OstreeSysroot> sysroot_;
};

}  // namespace OSTree

#endif  // AKTUALIZR_LITE_OSTREE_H_
