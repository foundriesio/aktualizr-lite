#include "aktualizr-lite/cli/cli.h"

#include <iostream>
#include <unordered_map>

#include "aktualizr-lite/api.h"

#include "aktualizr-lite/aklite_client_ext.h"
#include "logging/logging.h"

namespace aklite::cli {

using SC = StatusCode;

template <typename T>
static StatusCode res2StatusCode(const std::unordered_map<T, StatusCode> code_map, T rc) {
  if (code_map.count(rc) == 1) {
    return code_map.at(rc);
  }
  return SC::UnknownError;
}

static const std::unordered_map<CheckInResult::Status, StatusCode> c2s = {
    {CheckInResult::Status::Ok, SC::Ok},
    {CheckInResult::Status::OkCached, SC::CheckinOkCached},
    {CheckInResult::Status::Failed, SC::CheckinFailure},
    {CheckInResult::Status::NoMatchingTargets, SC::CheckinNoMatchingTargets},
    {CheckInResult::Status::NoTargetContent, SC::CheckinNoTargetContent},
    {CheckInResult::Status::SecurityError, SC::CheckinSecurityError},
    {CheckInResult::Status::ExpiredMetadata, SC::CheckinExpiredMetadata},
    {CheckInResult::Status::MetadataFetchFailure, SC::CheckinMetadataFetchFailure},
    {CheckInResult::Status::MetadataNotFound, SC::CheckinMetadataNotFound},
    {CheckInResult::Status::BundleMetadataError, SC::CheckinInvalidBundleMetadata},
};

static const std::unordered_map<GetTargetToInstallResult::Status, StatusCode> t2s = {
    {GetTargetToInstallResult::Status::TufTargetNotFound, SC::TufTargetNotFound},
    {GetTargetToInstallResult::Status::TargetAlreadyInstalled, SC::InstallAlreadyInstalled},
    {GetTargetToInstallResult::Status::RollbackTargetNotFound, SC::RollbackTargetNotFound},

    // Internal Issues
    {GetTargetToInstallResult::Status::RollbackTargetNotFound, SC::UnknownError},
    {GetTargetToInstallResult::Status::BadCheckinStatus, SC::UnknownError},

    // Success results
    {GetTargetToInstallResult::Status::NoUpdate, SC::Ok},
    {GetTargetToInstallResult::Status::UpdateNewVersion, SC::CheckinUpdateNewVersion},
    {GetTargetToInstallResult::Status::UpdateSyncApps, SC::CheckinUpdateSyncApps},
    {GetTargetToInstallResult::Status::UpdateRollback, SC::CheckinUpdateRollback},
};

static const std::unordered_map<DownloadResult::Status, StatusCode> d2s = {
    {DownloadResult::Status::Ok, SC::Ok},
    {DownloadResult::Status::DownloadFailed, SC::DownloadFailure},
    {DownloadResult::Status::VerificationFailed, SC::DownloadFailureVerificationFailed},
    {DownloadResult::Status::DownloadFailed_NoSpace, SC::DownloadFailureNoSpace},
};

static const std::unordered_map<InstallResult::Status, StatusCode> i2s = {
    {InstallResult::Status::Ok, SC::Ok},
    {InstallResult::Status::OkBootFwNeedsCompletion, SC::OkNeedsRebootForBootFw},
    {InstallResult::Status::NeedsCompletion, SC::InstallNeedsReboot},
    {InstallResult::Status::AppsNeedCompletion, SC::InstallAppsNeedFinalization},
    {InstallResult::Status::BootFwNeedsCompletion, SC::InstallNeedsRebootForBootFw},
    {InstallResult::Status::DownloadFailed, SC::InstallAppPullFailure},
    {InstallResult::Status::DownloadOstreeFailed, SC::DownloadFailure},
    {InstallResult::Status::VerificationFailed, SC::DownloadFailureVerificationFailed},
    {InstallResult::Status::DownloadFailed_NoSpace, SC::DownloadFailureNoSpace},
    {InstallResult::Status::InstallationInProgress, SC::InstallationInProgress},
    {InstallResult::Status::InstallRollbackFailed, SC::InstallRollbackFailed},
    {InstallResult::Status::InstallRollbackOk, SC::InstallRollbackOk},
    {InstallResult::Status::UnknownError, SC::UnknownError},
};

bool IsSuccessCode(StatusCode status) {
  return (status == SC::Ok || status == SC::CheckinOkCached || status == SC::CheckinUpdateNewVersion ||
          status == SC::CheckinUpdateSyncApps || status == SC::CheckinUpdateRollback ||
          status == SC::OkNeedsRebootForBootFw || status == SC::InstallNeedsReboot ||
          status == SC::InstallAppsNeedFinalization);
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

StatusCode CheckIn(AkliteClientExt &client, const LocalUpdateSource *local_update_source, CheckMode check_mode) {
  auto cr = checkIn(client, check_mode, local_update_source);
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
  if (!cr) {
    return res2StatusCode<CheckInResult::Status>(c2s, cr.status);
  }

  auto gti_res = client.GetTargetToInstall(cr);
  if (gti_res && gti_res.selected_target.IsUnknown()) {
    // Keep Ok vs OkCached differentiation in case of success and there is no target to install
    return res2StatusCode<CheckInResult::Status>(c2s, cr.status);
  } else {
    return res2StatusCode<GetTargetToInstallResult::Status>(t2s, gti_res.status);
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
    return SC::InstallationInProgress;
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
      return SC::InstallDowngradeAttempt;
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
    return SC::NoPendingInstallation;
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
        return SC::InstallRollbackOk;
      }
      const auto rir = ri->Install();
      if (rir.status == InstallResult::Status::Ok) {
        LOG_INFO << "Apps have been synced, rollback to " << current.Name() << " completed";
        return SC::InstallRollbackOk;
      } else {
        LOG_ERROR << "Failed to sync Apps, rollback to " << current.Name() << " failed";
        LOG_ERROR << "Try to install the current Target again: " << current.Name();
        return SC::InstallRollbackFailed;
      }
    } else {
      LOG_INFO << "Installation has failed, device was successfully booted on the updated rootfs but failed to start "
                  "the updated Apps";
      LOG_INFO << "Looking for Target to rollback to...";
      const auto rollback_target = client.GetRollbackTarget();
      if (rollback_target.IsUnknown()) {
        LOG_ERROR << "Failed to find the Target to rollback to, try to install another Target";
        return SC::InstallRollbackFailed;
      }
      LOG_INFO << "Rolling back to " << rollback_target.Name() << "...";
      auto ri = client.Installer(rollback_target);
      if (ri == nullptr) {
        LOG_ERROR
            << "Unexpected error: installer couldn't find the rollback Target in the DB; try to install another Target";
        return SC::UnknownError;
      }
      const auto rir = ri->Install();
      if (rir.status == InstallResult::Status::NeedsCompletion) {
        LOG_INFO << "Successfully installed the rollback Target, reboot is required to complete it";
        return SC::InstallRollbackNeedsReboot;
      }
      LOG_ERROR << "Failed to rollback to " << rollback_target.Name() << " try to install another Target";
      return SC::InstallRollbackFailed;
    }
  } else if (ir.status == InstallResult::Status::OkBootFwNeedsCompletion) {
    LOG_INFO << "Finalization was successful, reboot is required to confirm boot fw update";
  } else if (ir.status == InstallResult::Status::NeedsCompletion) {
    LOG_INFO << "Install finalization wasn't invoked, device reboot is required";
  }

  return res2StatusCode<InstallResult::Status>(i2s, ir.status);
}

}  // namespace aklite::cli
