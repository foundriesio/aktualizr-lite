#include "sysroot.h"
#include "logging/logging.h"

#include <boost/lexical_cast.hpp>

#include "ostree/repo.h"

namespace OSTree {

const unsigned int Sysroot::Config::DefaultStorageWatermark{90};
const unsigned int Sysroot::Config::MinStorageWatermark{50};
const unsigned int Sysroot::Config::MaxStorageWatermark{95};

Sysroot::Config::Config(const PackageConfig& pconfig) {
  if (pconfig.extra.count(StorageWatermarkParamName) == 1) {
    const std::string val_str{pconfig.extra.at(StorageWatermarkParamName)};
    try {
      const auto val{boost::lexical_cast<unsigned int>(val_str)};
      if (val < MinStorageWatermark) {
        LOG_ERROR << "Value of `" << StorageWatermarkParamName << "` parameter is too low: " << val_str
                  << "; setting it the minimum allowed: " << MinStorageWatermark;
        StorageWatermark = MinStorageWatermark;
      } else if (val > MaxStorageWatermark) {
        LOG_ERROR << "Value of `" << StorageWatermarkParamName << "` parameter is too high: " << val_str
                  << "; setting it the maximum allowed: " << MaxStorageWatermark;
        StorageWatermark = MaxStorageWatermark;
      } else {
        StorageWatermark = val;
      }
    } catch (const std::exception& exc) {
      LOG_ERROR << "Invalid value of `" << StorageWatermarkParamName << "` parameter: " << val_str
                << "; setting it the default value: " << DefaultStorageWatermark;
    }
  }
}

Sysroot::Sysroot(const PackageConfig& pconfig, std::string sysroot_path, BootedType booted, std::string os_name)
    : cfg_{pconfig},
      path_{std::move(sysroot_path)},
      repo_path_{path_ + "/ostree/repo"},
      booted_{booted},
      os_name_{std::move(os_name)},
      deployment_path_{path_ + "/ostree/deploy/" + os_name_ + "/deploy"} {
  sysroot_ = OstreeManager::LoadSysroot(path_);
}

bool Sysroot::reload() {
  // Just reload for the booted env. In non-booted env "pending" deployment becomes "current" just after installation
  // without a need to reboot. It in turns invalidates "getCurrent" value for tests at the stage after installation and
  // before reboot.
  if (booted_ == BootedType::kBooted) {
    return static_cast<bool>(ostree_sysroot_load_if_changed(sysroot_.get(), nullptr, nullptr, nullptr));
  }
  return true;
}

std::string Sysroot::getDeploymentHash(Deployment deployment_type) const {
  std::string deployment_hash;
  g_autoptr(GPtrArray) deployments = nullptr;
  OstreeDeployment* deployment = nullptr;

  switch (booted_) {
    case BootedType::kBooted:
      deployment = getDeploymentIfBooted(sysroot_.get(), os_name_.c_str(), deployment_type);
      break;
    case BootedType::kStaged:
      if (deployment_type == Deployment::kPending) {
        OstreeDeployment* cur_deployment = getDeploymentIfStaged(sysroot_.get(), os_name_.c_str(), deployment_type);
        // Load the sysroot to make sure we get its latest state, so we can get real "pending" deployment caused by
        // successful installation
        GObjectUniquePtr<OstreeSysroot> changed_sysroot = OstreeManager::LoadSysroot(path_);
        OstreeDeployment* pend_deployment =
            getDeploymentIfStaged(changed_sysroot.get(), os_name_.c_str(), deployment_type);
        deployment =
            (strcmp(ostree_deployment_get_csum(pend_deployment), ostree_deployment_get_csum(cur_deployment)) == 0)
                ? nullptr
                : pend_deployment;
      } else {
        deployment = getDeploymentIfStaged(sysroot_.get(), os_name_.c_str(), deployment_type);
      }
      break;
    default:
      throw std::runtime_error("Invalid boot type: " + std::to_string(static_cast<int>(booted_)));
  }

  if (deployment != nullptr) {
    deployment_hash = ostree_deployment_get_csum(deployment);
  }
  return deployment_hash;
}

OstreeDeployment* Sysroot::getDeploymentIfBooted(OstreeSysroot* sysroot, const char* os_name,
                                                 Deployment deployment_type) {
  OstreeDeployment* deployment{nullptr};

  switch (deployment_type) {
    case Deployment::kCurrent:
      deployment = ostree_sysroot_get_booted_deployment(sysroot);
      break;
    case Deployment::kPending:
      ostree_sysroot_query_deployments_for(sysroot, os_name, &deployment, nullptr);
      break;
    case Deployment::kRollback:
      ostree_sysroot_query_deployments_for(sysroot, os_name, nullptr, &deployment);
      break;
    default:
      throw std::runtime_error("Invalid deployment type: " + std::to_string(static_cast<int>(deployment_type)));
  }

  return deployment;
}

OstreeDeployment* Sysroot::getDeploymentIfStaged(OstreeSysroot* sysroot, const char* os_name,
                                                 Deployment deployment_type) {
  g_autoptr(GPtrArray) deployments{nullptr};
  OstreeDeployment* deployment{nullptr};

  switch (deployment_type) {
    case Deployment::kCurrent:
    case Deployment::kPending:
      // if non-booted env then "current" and "pending" deployment are actually the same
      ostree_sysroot_query_deployments_for(sysroot, os_name, &deployment, nullptr);
      break;
    case Deployment::kRollback:
      deployments = ostree_sysroot_get_deployments(sysroot);
      if (deployments != nullptr && deployments->len > 1) {
        // Rollback deployment is the second deployment in the array of deployments,
        // it goes just after the pending or current deployment if it's not booted sysroot.
        // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic)
        deployment = static_cast<OstreeDeployment*>(deployments->pdata[1]);
        if (strcmp(ostree_deployment_get_osname(deployment), os_name) != 0) {
          LOG_WARNING << "Found rollback deployment doesn't match the given os name; found: "
                      << ostree_deployment_get_osname(deployment) << ", expected: " << os_name;
          deployment = nullptr;
        }
      }
      break;
    default:
      throw std::runtime_error("Invalid deployment type: " + std::to_string(static_cast<int>(deployment_type)));
  }

  return deployment;
}

}  // namespace OSTree
