#include <aktualizr-lite/api.h>

#include "logging/logging.h"

namespace cli {

ExitCode Install(AkliteClient& client, int version) {
  ExitCode exit_code{ExitCode::UnknownError};

  TufTarget target{client.GetPendingTarget()};
  if (!target.IsUnknown()) {
    LOG_INFO << "Cannot install because there is another installation in progress: " << target.Name();
    return ExitCode::InstallationInProgress;
  }

  const auto current{client.GetCurrent()};
  const CheckInResult get_tuf_res{client.CheckIn()};
  if (get_tuf_res.status == CheckInResult::Status::Failed) {
    return ExitCode::TufMetaPullFailure;
  }

  if (version == -1) {
    target = get_tuf_res.GetLatest();
  } else {
    for (const auto& t : get_tuf_res.Targets()) {
      if (t.Version() == version) {
        target = t;
        break;
      }
    }
  }

  if (target.IsUnknown()) {
    LOG_INFO << "No Target found; version: " << (version == -1 ? "latest" : std::to_string(version))
             << ", hardware ID: " << client.GetConfig().get("provision.primary_ecu_hardware_id", "")
             << ", tag: " << client.GetConfig().get("pacman.tags", "");
    return ExitCode::TufTargetNotFound;
  }

  LOG_INFO << "Found Target: " << target.Name();
  auto installer = client.Installer(target, "", "");

  auto donwload_res = installer->Download();
  switch (donwload_res.status) {
    case DownloadResult::Status::DownloadFailed: {
      exit_code = ExitCode::DownloadFailure;
      break;
    }
    case DownloadResult::Status::VerificationFailed: {
      exit_code = ExitCode::DownloadFailureVerificationFailed;
      break;
    }
    case DownloadResult::Status::DownloadFailed_NoSpace: {
      exit_code = ExitCode::DownloadFailureNoSpace;
      break;
    }
    default: {
      exit_code = ExitCode::UnknownError;
      break;
    }
  }

  if (!donwload_res) {
    return exit_code;
  }

  auto install_res = installer->Install();
  switch (install_res.status) {
    case InstallResult::Status::Ok: {
      exit_code = ExitCode::Ok;
      break;
    }
    case InstallResult::Status::NeedsCompletion: {
      if (target == client.GetPendingTarget()) {
        exit_code = ExitCode::InstallNeedsReboot;
      } else {
        // If the given Target is not pending it means that installation was rejected
        // because the previous bootloader update require device rebooting to confirm the update.
        exit_code = ExitCode::InstallNeedsRebootForBootFw;
      }
      break;
    }
    case InstallResult::Status::Failed: {
      // do rollback
      auto rollback_installer = client.Installer(current, "", "");
      auto rollback_install_res = rollback_installer->Install();
      if (rollback_install_res.status == InstallResult::Status::Ok) {
        exit_code = ExitCode::InstallRollbackOk;
      } else {
        exit_code = ExitCode::InstallRollbackFailed;
      }
      break;
    }
    default: {
      // TODO
      break;
    }
  }

  // TODO
  return exit_code;
}

ExitCode CompleteInstall(AkliteClient& client) {
  ExitCode exit_code{ExitCode::UnknownError};
  TufTarget target{client.GetPendingTarget()};

  if (target.IsUnknown()) {
    LOG_INFO << "There is no pending installation to complete";
    return ExitCode::NoPendingInstallation;
  }

  auto install_res = client.CompleteInstallation();
  switch (install_res.status) {
    case InstallResult::Status::Ok: {
      exit_code = ExitCode::Ok;
      break;
    }
    case InstallResult::Status::NeedsCompletion: {
      // Update of sotree and Apps have been completed successfully;
      // bootloader was updated too and it requires device reboot to confirm its update.
      exit_code = ExitCode::InstallNeedsRebootForBootFw;
      break;
    }
    case InstallResult::Status::Failed: {
      // do rollback
      // check rollback type, bootloader (boot on the previous version) or App driven
      const auto current{client.GetCurrent()};  // returns Target a device is booted on
      if (current.Sha256Hash() != target.Sha256Hash()) {
        // ostree rollback
        auto rollback_installer = client.CheckAppsInSync();
        if (rollback_installer) {
          auto rollback_download_res = rollback_installer->Download();
          // TODO
          auto rollback_install_res = rollback_installer->Install();
          if (rollback_install_res.status == InstallResult::Status::Ok) {
            exit_code = ExitCode::InstallRollbackOk;
          } else {
            exit_code = ExitCode::InstallRollbackFailed;
          }
        } else {
          exit_code = ExitCode::InstallRollbackOk;
        }
      } else {
        const auto rollback_target{client.GetRollbackTarget()};
        auto rollback_installer = client.Installer(rollback_target, "", "");
        auto rollback_download_res = rollback_installer->Download();
        // TODO
        auto rollback_install_res = rollback_installer->Install();
        switch (rollback_install_res.status) {
          case InstallResult::Status::Ok: {
            exit_code = ExitCode::Ok;
            break;
          }
          case InstallResult::Status::NeedsCompletion: {
            if (rollback_target == client.GetPendingTarget()) {
              exit_code = ExitCode::InstallRollbackNeedsReboot;
            } else {
              // TODO
            }
            break;
          }
          case InstallResult::Status::Failed: {
            exit_code = ExitCode::InstallRollbackFailed;
            break;
          }
          default: {
            // TODO
            break;
          }
        }
      }
      break;
    }
    default:
      // TODO
      break;
  }

  // TODO
  return exit_code;
}

}  // namespace cli
