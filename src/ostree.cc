#include "ostree.h"

namespace OSTree {

Sysroot::Sysroot(const std::string& sysroot_path, bool booted) : path_{sysroot_path}, booted_{booted} {
  sysroot_ = OstreeManager::LoadSysroot(path_);
}

std::string Sysroot::getCurDeploymentHash() const {
  OstreeDeployment* deployment = nullptr;

  if (booted_) {
    deployment = ostree_sysroot_get_booted_deployment(sysroot_.get());
  } else {
    GPtrArray* deployments = nullptr;
    deployments = ostree_sysroot_get_deployments(sysroot_.get());
    if (deployments != nullptr && deployments->len > 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      deployment = static_cast<OstreeDeployment*>(deployments->pdata[0]);
    }
  }

  if (deployment == nullptr) {
    LOG_WARNING << "Failed to get " << type() << " deployment in " << path();
    return "";
  }
  return ostree_deployment_get_csum(deployment);
}

}  // namespace OSTree
