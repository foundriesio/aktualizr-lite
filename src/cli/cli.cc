#include "aktualizr-lite/cli/cli.h"

#include <iostream>
#include <unordered_map>

#include "aktualizr-lite/api.h"

#include "logging/logging.h"

namespace aklite::cli {

template <typename T>
static StatusCode res2StatusCode(const std::unordered_map<T, StatusCode> code_map, T rc) {
  if (code_map.count(rc) == 1) {
    return code_map.at(rc);
  }
  return StatusCode::UnknownError;
}

static const std::unordered_map<CheckInResult::Status, StatusCode> c2s = {
    {CheckInResult::Status::Ok, StatusCode::Ok},
    {CheckInResult::Status::OkCached, StatusCode::CheckinOkCached},
    {CheckInResult::Status::Failed, StatusCode::CheckinFailure},
    {CheckInResult::Status::NoMatchingTargets, StatusCode::CheckinNoMatchingTargets},
    {CheckInResult::Status::NoTargetContent, StatusCode::CheckinNoTargetContent},
    {CheckInResult::Status::SecurityError, StatusCode::CheckinSecurityError},
    {CheckInResult::Status::ExpiredMetadata, StatusCode::CheckinExpiredMetadata},
    {CheckInResult::Status::MetadataFetchFailure, StatusCode::CheckinMetadataFetchFailure},
    {CheckInResult::Status::MetadataNotFound, StatusCode::CheckinMetadataNotFound},
    {CheckInResult::Status::BundleMetadataError, StatusCode::CheckinInvalidBundleMetadata},
};

static const std::unordered_map<DownloadResult::Status, StatusCode> d2s = {
    {DownloadResult::Status::Ok, StatusCode::Ok},
    {DownloadResult::Status::DownloadFailed, StatusCode::DownloadFailure},
    {DownloadResult::Status::VerificationFailed, StatusCode::DownloadFailureVerificationFailed},
    {DownloadResult::Status::DownloadFailed_NoSpace, StatusCode::DownloadFailureNoSpace},
};

static const std::unordered_map<InstallResult::Status, StatusCode> i2s = {
    {InstallResult::Status::Ok, StatusCode::Ok},
    {InstallResult::Status::OkBootFwNeedsCompletion, StatusCode::OkNeedsRebootForBootFw},
    {InstallResult::Status::NeedsCompletion, StatusCode::InstallNeedsReboot},
    {InstallResult::Status::AppsNeedCompletion, StatusCode::InstallAppsNeedFinalization},
    {InstallResult::Status::BootFwNeedsCompletion, StatusCode::InstallNeedsRebootForBootFw},
    {InstallResult::Status::DownloadFailed, StatusCode::InstallAppPullFailure},
};

bool IsSuccessCode(StatusCode status) {
  return (status == StatusCode::Ok || status == StatusCode::CheckinOkCached ||
          status == StatusCode::OkNeedsRebootForBootFw || status == StatusCode::InstallNeedsReboot ||
          status == StatusCode::InstallAppsNeedFinalization);
}

StatusCode CheckIn(AkliteClient &client, const LocalUpdateSource *local_update_source) {
  CheckInResult cr{CheckInResult::Status::Failed, "", std::vector<TufTarget>{}};
  if (local_update_source == nullptr) {
    cr = client.CheckIn();
  } else {
    cr = client.CheckInLocal(local_update_source);
  }
  if (cr) {
    if (cr.Targets().empty()) {
      std::cout << "\nNo Targets found" << std::endl;
    } else {
      std::cout << "\nFound Targets: " << std::endl;
    }
    for (const auto &t : cr.Targets()) {
      std::cout << "\tName: " << t.Name() << std::endl;
      std::cout << "\tOSTree hash: " << t.Sha256Hash() << std::endl;
      std::cout << "\tApps:" << std::endl;
      for (const auto &a : TufTarget::Apps(t)) {
        std::cout << "\t\t" << a.name << " -> " << a.uri << std::endl;
      }
      std::cout << std::endl;
    }
  }
  return res2StatusCode<CheckInResult::Status>(c2s, cr.status);
}

static StatusCode pullAndInstall(AkliteClient &client, int version, const std::string &target_name,
                                 InstallMode install_mode, bool force_downgrade,
                                 const LocalUpdateSource *local_update_source, PullMode pull_mode, bool do_install) {
  // Check if the device is in a correct state to start a new update
  if (client.IsInstallationInProgress()) {
    LOG_ERROR << "Cannot start Target installation since there is ongoing installation; target: "
              << client.GetPendingTarget().Name();
    return StatusCode::InstallationInProgress;
  }

  const auto current{client.GetCurrent()};
  CheckInResult cr{CheckInResult::Status::Failed, "", std::vector<TufTarget>{}};
  if (local_update_source == nullptr) {
    cr = client.CheckIn();
  } else {
    cr = client.CheckInLocal(local_update_source);
  }
  if (!cr) {
    return res2StatusCode<CheckInResult::Status>(c2s, cr.status);
  }

  auto target = cr.SelectTarget(version, target_name);
  if (target.IsUnknown()) {
    std::string target_string;
    if (version == -1 && target_name.empty()) {
      target_string = " version: latest,";
    } else {
      if (!target_name.empty()) {
        target_string = " name: " + target_name + ",";
      }
      if (version != -1) {
        target_string += " version: " + std::to_string(version) + ",";
      }
    }

    LOG_ERROR << "No Target found;" << target_string
              << " hardware ID: " << client.GetConfig().get("provision.primary_ecu_hardware_id", "")
              << ", tag: " << client.GetConfig().get("pacman.tags", "");
    return StatusCode::TufTargetNotFound;
  }

  if (current.Version() > target.Version()) {
    LOG_WARNING << "Found TUF Target is lower version than the current on; "
                << "current: " << current.Version() << ", found Target: " << target.Version();

    if (!force_downgrade) {
      LOG_ERROR << "Downgrade is not allowed by default, re-run the command with `--force` option to force downgrade";
      return StatusCode::InstallDowngradeAttempt;
    }
    LOG_WARNING << "Downgrading from " << current.Version() << " to  " << target.Version() << "...";
  }

  // Check whether the given target is already installed and synced/running
  if (current == target && client.CheckAppsInSync() == nullptr) {
    if (local_update_source != nullptr) {
      return StatusCode::InstallAlreadyInstalled;
    } else {
      LOG_INFO
          << "The specified Target is already installed, enforcing installation to make sure it's synced and running: "
          << target.Name();
    }
  } else {
    // Run the target installation
    LOG_INFO << "Updating Active Target: " << current.Name();
    LOG_INFO << "To New Target: " << target.Name();
  }

  const auto installer = client.Installer(target, "", "", install_mode, local_update_source);
  if (installer == nullptr) {
    LOG_ERROR << "Unexpected error: installer couldn't find Target in the DB; try again later";
    return StatusCode::UnknownError;
  }

  if (pull_mode == PullMode::All) {
    auto dr = installer->Download();
    if (!dr) {
      LOG_ERROR << "Failed to download Target; target: " << target.Name() << ", err: " << dr;
      return res2StatusCode<DownloadResult::Status>(d2s, dr.status);
    }

    if (!do_install) {
      return res2StatusCode<DownloadResult::Status>(d2s, dr.status);
    }
  }

  auto ir = installer->Install();
  if (!ir) {
    LOG_ERROR << "Failed to install Target; target: " << target.Name() << ", err: " << ir;
    if (ir.status == InstallResult::Status::Failed) {
      LOG_INFO << "Rolling back to the previous target: " << current.Name() << "...";
      const auto installer = client.Installer(current);
      if (installer == nullptr) {
        LOG_ERROR << "Failed to find the previous target in the TUF Targets DB";
        return StatusCode::InstallRollbackFailed;
      }
      ir = installer->Install();
      if (!ir) {
        LOG_ERROR << "Failed to rollback to " << current.Name() << ", err: " << ir;
      }
      if (ir.status == InstallResult::Status::Ok) {
        return StatusCode::InstallRollbackOk;
      } else {
        return StatusCode::InstallRollbackFailed;
      }
    }
  }

  return res2StatusCode<InstallResult::Status>(i2s, ir.status);
}

StatusCode Pull(AkliteClient &client, int version, const std::string &target_name, bool force_downgrade,
                const LocalUpdateSource *local_update_source) {
  return pullAndInstall(client, version, target_name, InstallMode::All, force_downgrade, local_update_source,
                        PullMode::All, false);
}

StatusCode Install(AkliteClient &client, int version, const std::string &target_name, InstallMode install_mode,
                   bool force_downgrade, const LocalUpdateSource *local_update_source, PullMode pull_mode) {
  return pullAndInstall(client, version, target_name, install_mode, force_downgrade, local_update_source, pull_mode,
                        true);
}

StatusCode CompleteInstall(AkliteClient &client) {
  if (!client.IsInstallationInProgress()) {
    LOG_ERROR << "There is no pending installation to complete";
    return StatusCode::NoPendingInstallation;
  }
  const auto pending{client.GetPendingTarget()};  // returns Target that the device was supposed to boot on
  const auto ir = client.CompleteInstallation();
  if (!ir) {
    LOG_ERROR << "Failed to finalize pending installation; target: " << pending.Name() << ", err: " << ir;

    // check rollback type, the bootloader or App driven
    const auto current{client.GetCurrent()};  // returns Target the device is booted on
    if (current.Sha256Hash() != pending.Sha256Hash()) {
      // ostree rollback, aka the bootloader driven rollback
      LOG_INFO << "Installation has failed, device was rolled back to " << current.Name();
      LOG_INFO << "Syncing Apps with the Target that device was rolled back to if needed...";
      auto ri = client.CheckAppsInSync();
      if (!ri) {
        // ostree rollback and no need to sync Apps since the rollback target eithe doesn't have Apps or
        // its Apps were not updated hence are already running.
        LOG_INFO << "No Apps to sync, rollback to " << current.Name() << " completed";
        return StatusCode::InstallRollbackOk;
      }
      const auto rir = ri->Install();
      if (rir.status == InstallResult::Status::Ok) {
        LOG_INFO << "Apps have been synced, rollback to " << current.Name() << " completed";
        return StatusCode::InstallRollbackOk;
      } else {
        LOG_ERROR << "Failed to sync Apps, rollback to " << current.Name() << " failed";
        LOG_ERROR << "Try to install the current Target again: " << current.Name();
        return StatusCode::InstallRollbackFailed;
      }
    } else {
      LOG_INFO << "Installation has failed, device was successfully booted on the updated rootfs but failed to start "
                  "the updated Apps";
      LOG_INFO << "Looking for Target to rollback to...";
      const auto rollback_target = client.GetRollbackTarget();
      if (rollback_target.IsUnknown()) {
        LOG_ERROR << "Failed to find the Target to rollback to, try to install another Target";
        return StatusCode::InstallRollbackFailed;
      }
      LOG_INFO << "Rolling back to " << rollback_target.Name() << "...";
      auto ri = client.Installer(rollback_target);
      if (ri == nullptr) {
        LOG_ERROR
            << "Unexpected error: installer couldn't find the rollback Target in the DB; try to install another Target";
        return StatusCode::UnknownError;
      }
      const auto rir = ri->Install();
      if (rir.status == InstallResult::Status::NeedsCompletion) {
        LOG_INFO << "Successfully installed the rollback Target, reboot is required to complete it";
        return StatusCode::InstallRollbackNeedsReboot;
      }
      LOG_ERROR << "Failed to rollback to " << rollback_target.Name() << " try to install another Target";
      return StatusCode::InstallRollbackFailed;
    }
  } else if (ir.status == InstallResult::Status::OkBootFwNeedsCompletion) {
    LOG_INFO << "Finalization was successful, reboot is required to confirm boot fw update";
  } else if (ir.status == InstallResult::Status::NeedsCompletion) {
    LOG_INFO << "Install finalization wasn't invoked, device reboot is required";
  }

  return res2StatusCode<InstallResult::Status>(i2s, ir.status);
}

}  // namespace aklite::cli
