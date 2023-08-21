#include "sysroot.h"
#include "logging/logging.h"

#include <boost/lexical_cast.hpp>

#include "ostree/repo.h"

namespace OSTree {

const unsigned int Sysroot::Config::DefaultStorageWatermark{90};
const unsigned int Sysroot::Config::MinStorageWatermark{50};
const unsigned int Sysroot::Config::MaxStorageWatermark{95};

const unsigned int Sysroot::Config::MinFreeSpacePercent{3};
const unsigned int Sysroot::Config::MaxFreeSpacePercent{50};

Sysroot::Config::Config(const PackageConfig& pconfig) {
  Path = pconfig.sysroot.string();
  Type = pconfig.booted;
  OsName = pconfig.os.empty() ? "lmp" : pconfig.os;

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

  if (pconfig.extra.count(StorageFreeSpacePercentParamName) == 1) {
    const std::string val_str{pconfig.extra.at(StorageFreeSpacePercentParamName)};
    try {
      const auto val{boost::lexical_cast<unsigned int>(val_str)};
      if (val < MinFreeSpacePercent) {
        LOG_ERROR << "Value of `" << StorageFreeSpacePercentParamName << "` parameter is too low: " << val_str
                  << "; won't override the value set in the ostree config";
      } else if (val > MaxFreeSpacePercent) {
        LOG_ERROR << "Value of `" << StorageFreeSpacePercentParamName << "` parameter is too high: " << val_str
                  << "; won't override the value set in the ostree config";
      } else {
        StorageFreeSpacePercent = val;
      }
    } catch (const std::exception& exc) {
      LOG_ERROR << "Invalid value of `" << StorageFreeSpacePercentParamName << "` parameter: " << val_str
                << "; won't override the value set in the ostree config";
    }
  }
}

Sysroot::Sysroot(const PackageConfig& pconfig)
    : cfg_{pconfig},
      repo_path_{cfg_.Path + "/ostree/repo"},
      deployment_path_{cfg_.Path + "/ostree/deploy/" + cfg_.OsName + "/deploy"} {
  Repo repo{repo_path_};
  const auto ostree_min_free_space{repo.getFreeSpacePercent()};
  if (-1 == cfg_.StorageFreeSpacePercent) {
    LOG_DEBUG
        << "Minimum free space percentage is not overridden, applying the one that is configured in the ostree config: "
        << ostree_min_free_space;
  } else {
    try {
      repo.setFreeSpacePercent(cfg_.StorageFreeSpacePercent);
      LOG_DEBUG << "Minimum free space percentage is overridden; "
                << "from " << ostree_min_free_space << " to  " << cfg_.StorageFreeSpacePercent;
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to override `min-free-space-percent` value in the ostree config, applying the one that is "
                   "configured in the ostree config: "
                << ostree_min_free_space << "; err: " << exc.what();
    }
  }
  sysroot_ = OstreeManager::LoadSysroot(cfg_.Path);
}

bool Sysroot::reload() {
  // Just reload for the booted env. In non-booted env "pending" deployment becomes "current" just after installation
  // without a need to reboot. It in turns invalidates "getCurrent" value for tests at the stage after installation and
  // before reboot.
  if (cfg_.Type == BootedType::kBooted) {
    return static_cast<bool>(ostree_sysroot_load_if_changed(sysroot_.get(), nullptr, nullptr, nullptr));
  }
  return true;
}

std::string Sysroot::getDeploymentHash(Deployment deployment_type) const {
  std::string deployment_hash;
  g_autoptr(GPtrArray) deployments = nullptr;
  OstreeDeployment* deployment = nullptr;

  switch (cfg_.Type) {
    case BootedType::kBooted:
      deployment = getDeploymentIfBooted(sysroot_.get(), cfg_.OsName.c_str(), deployment_type);
      break;
    case BootedType::kStaged:
      if (deployment_type == Deployment::kPending) {
        OstreeDeployment* cur_deployment = getDeploymentIfStaged(sysroot_.get(), cfg_.OsName.c_str(), deployment_type);
        // Load the sysroot to make sure we get its latest state, so we can get real "pending" deployment caused by
        // successful installation
        GObjectUniquePtr<OstreeSysroot> changed_sysroot = OstreeManager::LoadSysroot(cfg_.Path);
        OstreeDeployment* pend_deployment =
            getDeploymentIfStaged(changed_sysroot.get(), cfg_.OsName.c_str(), deployment_type);
        deployment =
            (strcmp(ostree_deployment_get_csum(pend_deployment), ostree_deployment_get_csum(cur_deployment)) == 0)
                ? nullptr
                : pend_deployment;
      } else {
        deployment = getDeploymentIfStaged(sysroot_.get(), cfg_.OsName.c_str(), deployment_type);
      }
      break;
    default:
      throw std::runtime_error("Invalid boot type: " + std::to_string(static_cast<int>(cfg_.Type)));
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
