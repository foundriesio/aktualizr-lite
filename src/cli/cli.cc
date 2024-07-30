#include "aktualizr-lite/cli/cli.h"

#include <iostream>
#include <unordered_map>

#include "aktualizr-lite/api.h"

#include "aktualizr-lite/aklite_client_ext.h"
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

static const std::unordered_map<GetTargetToInstallResult::Status, StatusCode> t2s = {
    {GetTargetToInstallResult::Status::TufTargetNotFound, StatusCode::TufTargetNotFound},
    {GetTargetToInstallResult::Status::TargetAlreadyInstalled, StatusCode::InstallAlreadyInstalled},
    {GetTargetToInstallResult::Status::RollbackTargetNotFound, StatusCode::RollbackTargetNotFound},

    // Internal Issues
    {GetTargetToInstallResult::Status::RollbackTargetNotFound, StatusCode::UnknownError},
    {GetTargetToInstallResult::Status::BadCheckinStatus, StatusCode::UnknownError},

    // Success results
    {GetTargetToInstallResult::Status::NoUpdate, StatusCode::Ok},
    {GetTargetToInstallResult::Status::UpdateNewVersion, StatusCode::CheckinUpdateNewVersion},
    {GetTargetToInstallResult::Status::UpdateSyncApps, StatusCode::CheckinUpdateSyncApps},
    {GetTargetToInstallResult::Status::UpdateRollback, StatusCode::CheckinUpdateRollback},
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
    {InstallResult::Status::DownloadOstreeFailed, StatusCode::DownloadFailure},
    {InstallResult::Status::VerificationFailed, StatusCode::DownloadFailureVerificationFailed},
    {InstallResult::Status::DownloadFailed_NoSpace, StatusCode::DownloadFailureNoSpace},
    {InstallResult::Status::InstallationInProgress, StatusCode::InstallationInProgress},
    {InstallResult::Status::InstallRollbackFailed, StatusCode::InstallRollbackFailed},
    {InstallResult::Status::InstallRollbackOk, StatusCode::InstallRollbackOk},
    {InstallResult::Status::UnknownError, StatusCode::UnknownError},
};

bool IsSuccessCode(StatusCode status) {
  return (status == StatusCode::Ok || status == StatusCode::CheckinOkCached ||
          status == StatusCode::CheckinUpdateNewVersion || status == StatusCode::CheckinUpdateSyncApps ||
          status == StatusCode::CheckinUpdateRollback || status == StatusCode::OkNeedsRebootForBootFw ||
          status == StatusCode::InstallNeedsReboot || status == StatusCode::InstallAppsNeedFinalization);
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
      std::cout << "\nNo Targets found"
                << "\n";
    } else {
      std::cout << "\nFound Targets: "
                << "\n";
    }
    auto app_shortlist = client.GetAppShortlist();
    for (const auto &target : cr.Targets()) {
      std::cout << target.Name() << "\n";
      std::cout << "\tostree: " << target.Sha256Hash() << "\n";
      std::cout << "\tapps:"
                << "\n";
      for (const auto &app : TufTarget::Apps(target)) {
        std::string app_status = (!app_shortlist || std::find(app_shortlist->begin(), app_shortlist->end(), app.name) !=
                                                        app_shortlist->end())
                                     ? "on"
                                     : "off";

        std::cout << "\t\t(" << app_status << ") " << app.name << " -> " << app.uri << "\n";
      }
      std::cout << "\n";
    }
  }
  return res2StatusCode<CheckInResult::Status>(c2s, cr.status);
}

static CheckInResult checkIn(AkliteClientExt &client, CheckMode check_mode,
                             const LocalUpdateSource *local_update_source) {
  if (check_mode == CheckMode::Update) {
    if (local_update_source == nullptr) {
      return client.CheckIn();
    } else {
      return client.CheckInLocal(local_update_source);
    }
  } else {
    return client.CheckInCurrent(local_update_source);
  }
}

static StatusCode pullAndInstall(AkliteClientExt &client, int version, const std::string &target_name,
                                 InstallMode install_mode, bool force_downgrade,
                                 const LocalUpdateSource *local_update_source, PullMode pull_mode, bool do_install,
                                 CheckMode check_mode) {
  // Check if the device is in a correct state to start a new update
  if (client.IsInstallationInProgress()) {
    LOG_ERROR << "Cannot start Target installation since there is ongoing installation; target: "
              << client.GetPendingTarget().Name();
    return StatusCode::InstallationInProgress;
  }

  const auto ci_res = checkIn(client, check_mode, local_update_source);
  if (!ci_res) {
    return res2StatusCode<CheckInResult::Status>(c2s, ci_res.status);
  }

  auto gti_res = client.GetTargetToInstall(ci_res, version, target_name, true, true, local_update_source != nullptr);

  //
  if (gti_res.selected_target.IsUnknown()) {
    if (gti_res.status == GetTargetToInstallResult::Status::TufTargetNotFound) {
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
    } else if (gti_res) {
      LOG_INFO << "No target to update";
    }
    return res2StatusCode<GetTargetToInstallResult::Status>(t2s, gti_res.status);
  }

  const auto current{client.GetCurrent()};
  if (current.Version() > gti_res.selected_target.Version()) {
    LOG_WARNING << "Found TUF Target is lower version than the current on; "
                << "current: " << current.Version() << ", found Target: " << gti_res.selected_target.Version();

    if (!force_downgrade) {
      LOG_ERROR << "Downgrade is not allowed by default, re-run the command with `--force` option to force downgrade";
      return StatusCode::InstallDowngradeAttempt;
    }
    LOG_WARNING << "Downgrading from " << current.Version() << " to  " << gti_res.selected_target.Version() << "...";
  }

  auto pi_res = client.PullAndInstall(gti_res.selected_target, "", "", install_mode, local_update_source,
                                      pull_mode == PullMode::All, do_install);
  return res2StatusCode<InstallResult::Status>(i2s, pi_res.status);
}

StatusCode Pull(AkliteClientExt &client, int version, const std::string &target_name, bool force_downgrade,
                const LocalUpdateSource *local_update_source, CheckMode check_mode) {
  return pullAndInstall(client, version, target_name, InstallMode::All, force_downgrade, local_update_source,
                        PullMode::All, false, check_mode);
}

StatusCode Install(AkliteClientExt &client, int version, const std::string &target_name, InstallMode install_mode,
                   bool force_downgrade, const LocalUpdateSource *local_update_source, PullMode pull_mode,
                   CheckMode check_mode) {
  return pullAndInstall(client, version, target_name, install_mode, force_downgrade, local_update_source, pull_mode,
                        true, check_mode);
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
