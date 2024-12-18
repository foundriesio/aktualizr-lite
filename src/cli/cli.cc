#include "aktualizr-lite/cli/cli.h"

#include <algorithm>
#include <iostream>
#include <unordered_map>

#include "aktualizr-lite/api.h"

#include "aktualizr-lite/aklite_client_ext.h"
#include "json/reader.h"
#include "json/value.h"
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

using SC = StatusCode;
static const std::unordered_map<SC, std::string> status2string = {
    {SC::Ok, "Operation succeeded"},

    // Return codes for CheckIn, Pull and Install
    {SC::CheckinOkCached, "Unable to fetch updated TUF metadata, but stored metadata is valid"},
    {SC::CheckinFailure, "Failed to update TUF metadata"},
    {SC::CheckinNoMatchingTargets,
     "There is no target in the device TUF repo that matches a device tag and/or hardware ID"},
    {SC::CheckinNoTargetContent,
     "Failed to find the ostree commit and/or all Apps of the Target to be installed in the provided source bundle "
     "(offline mode only)"},
    {SC::CheckinSecurityError, "Invalid TUF metadata"},
    {SC::CheckinExpiredMetadata, "TUF metadata is expired"},
    {SC::CheckinMetadataFetchFailure, "Unable to fetch TUF metadata"},
    {SC::CheckinMetadataNotFound, "TUF metadata not found in the provided path (offline mode only)"},
    {SC::CheckinInvalidBundleMetadata,
     "The bundle metadata is invalid (offline mode only)."
     "There are a few reasons why the metadata might be invalid:\n"
     "        1. One or more bundle signatures is/are invalid.\n"
     "        2. The bundle targets' type, whether CI or production, differs from the device's type.\n"
     "        3. The bundle targets' tag differs from the device's tag."},
    {SC::TufTargetNotFound, "Selected target not found"},
    {SC::CheckinUpdateNewVersion, "Update required: new version"},
    {SC::CheckinUpdateSyncApps, "Update required: apps synchronization"},
    {SC::CheckinUpdateRollback, "Update required: rollback"},

    // Return codes for Pull and Install
    {SC::RollbackTargetNotFound,
     "Unable to find target to rollback to after a failure to start Apps at boot on a new version of sysroot"},
    {SC::InstallationInProgress, "Unable to pull/install: there is an installation that needs completion"},
    {SC::DownloadFailure, "Unable to download target"},
    {SC::DownloadFailureNoSpace, "There is no enough free space to download the target"},
    {SC::DownloadFailureVerificationFailed,
     "The pulled target content is invalid, specifically App compose file is invalid"},
    {SC::InstallAlreadyInstalled, "Selected target is already installed"},
    {SC::InstallDowngradeAttempt, "Attempted to install a previous version"},

    // Return codes for Install
    {SC::InstallAppsNeedFinalization, "Execute the `run` subcommand to finalize installation"},
    {SC::InstallAppPullFailure, "Unable read target data, make sure it was pulled"},
    {SC::InstallNeedsRebootForBootFw,
     "Reboot is required to complete the previous boot firmware update. After reboot the update attempt must be "
     "repeated from the beginning"},

    // Return codes for Install and CompleteInstall
    {SC::InstallNeedsReboot, "Reboot to finalize installation"},
    {SC::OkNeedsRebootForBootFw, "Reboot to finalize bootloader installation"},
    {SC::InstallRollbackNeedsReboot, "Installation failed, rollback initiated but requires reboot to finalize"},

    // Return codes for CompleteInstall
    {SC::NoPendingInstallation, "No pending installation to run"},
    {SC::InstallOfflineRollbackOk, "Offline installation failed, rollback performed"},
    {SC::InstallRollbackOk, "Online installation failed, rollback performed"},
    {SC::InstallRollbackFailed, "Installation failed and rollback operation was not successful"},
    {SC::UnknownError, "Unknown error"},
};

std::string StatusCodeDescription(StatusCode status) {
  if (status2string.count(status) == 1) {
    return status2string.at(status);
  }
  return "Invalid StatusCode value: " + std::to_string(static_cast<int>(status));
}

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

static Json::Value getCheckInResultJson(AkliteClientExt &client, const CheckInResult &ci_res,
                                        const GetTargetToInstallResult &gti_res) {
  const auto current = client.GetCurrent();

  Json::Value json_root;

  auto app_shortlist = client.GetAppShortlist();
  for (const auto &target : ci_res.Targets()) {
    Json::Value json_target;
    json_target["name"] = target.Name();
    json_target["version"] = target.Version();
    if (!gti_res.selected_target.IsUnknown() && gti_res.selected_target.Name() == target.Name()) {
      json_target["selected"] = true;
      json_target["reason"] = gti_res.reason;
    }
    if (client.IsRollback(target)) {
      json_target["failed"] = true;
    }
    if (current.Version() == target.Version()) {
      json_target["current"] = true;
    } else if (current.Version() < target.Version()) {
      json_target["newer"] = true;
    }

    Json::Value json_apps;
    for (const auto &app : TufTarget::Apps(target)) {
      Json::Value json_app;
      json_app["name"] = app.name;
      json_app["uri"] = app.uri;
      json_app["on"] =
          (!app_shortlist || std::find(app_shortlist->begin(), app_shortlist->end(), app.name) != app_shortlist->end());
      json_apps.append(json_app);
    }
    json_target["apps"] = json_apps;

    json_root.append(json_target);
  }

  return json_root;
}

StatusCode CheckIn(AkliteClientExt &client, const LocalUpdateSource *local_update_source, CheckMode check_mode,
                   bool json_output) {
  auto cr = checkIn(client, check_mode, local_update_source);
  if (!cr) {
    return res2StatusCode<CheckInResult::Status>(c2s, cr.status);
  }

  auto gti_res = client.GetTargetToInstall(cr);

  if (json_output) {
    std::cout << getCheckInResultJson(client, cr, gti_res) << "\n";
  } else {
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

  auto pi_res = client.PullAndInstall(gti_res.selected_target, gti_res.reason, "", install_mode, local_update_source,
                                      pull_mode == PullMode::All, do_install,
                                      gti_res.status == GetTargetToInstallResult::Status::UpdateNewVersion);
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

StatusCode Rollback(AkliteClientExt &client, const LocalUpdateSource *local_update_source) {
  auto install_result = client.Rollback(local_update_source);
  return res2StatusCode<InstallResult::Status>(i2s, install_result.status);
}

}  // namespace aklite::cli
