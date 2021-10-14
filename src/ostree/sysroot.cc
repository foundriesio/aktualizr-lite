#include "sysroot.h"

namespace OSTree {

Sysroot::Sysroot(std::string sysroot_path, BootedType booted) : path_{std::move(sysroot_path)}, booted_{booted} {
  sysroot_ = OstreeManager::LoadSysroot(path_);
}

std::string Sysroot::getCurDeploymentHash() const {
  g_autoptr(GPtrArray) deployments = nullptr;
  OstreeDeployment* deployment = nullptr;

  if (booted_ == BootedType::kBooted) {
    deployment = ostree_sysroot_get_booted_deployment(sysroot_.get());
  } else {
    deployments = ostree_sysroot_get_deployments(sysroot_.get());
    if (deployments != nullptr && deployments->len > 0) {
      // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
      deployment = static_cast<OstreeDeployment*>(deployments->pdata[0]);
    }
  }

  if (deployment == nullptr) {
    LOG_WARNING << "Failed to get " << booted_ << " deployment in " << path();
    return "";
  }
  return ostree_deployment_get_csum(deployment);
}

}  // namespace OSTree
