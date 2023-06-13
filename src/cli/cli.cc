#include "cli.h"

#include <unordered_map>

#include "aktualizr-lite/api.h"

#include "logging/logging.h"

namespace cli {

template <typename T>
static StatusCode res2StatusCode(const std::unordered_map<T, StatusCode> code_map, T rc) {
  if (code_map.count(rc) == 1) {
    return code_map.at(rc);
  }
  return StatusCode::UnknownError;
}

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
    {InstallResult::Status::BootFwNeedsCompletion, StatusCode::InstallNeedsRebootForBootFw},
    {InstallResult::Status::DownloadFailed, StatusCode::InstallAppPullFailure},
};

StatusCode Install(AkliteClient &client, int version) {
  // Check if a device is in a correct state to start a new update
  if (client.IsInstallationInProgress()) {
    LOG_ERROR << "Cannot start Target installation since there is ongoing installation; target: "
              << client.GetPendingTarget().Name();
    return StatusCode::InstallationInProgress;
  }

  const auto current{client.GetCurrent()};
  const CheckInResult cr{client.CheckIn()};
  if (cr.status == CheckInResult::Status::Failed) {
    LOG_ERROR << "Failed to pull TUF metadata or they are invalid";
    return StatusCode::TufMetaPullFailure;
  }

  TufTarget target;
  if (version == -1) {
    target = cr.GetLatest();
  } else {
    for (const auto &t : cr.Targets()) {
      if (t.Version() == version) {
        target = t;
        break;
      }
    }
  }
  if (target.IsUnknown()) {
    LOG_ERROR << "No Target found; version: " << (version == -1 ? "latest" : std::to_string(version))
              << ", hardware ID: " << client.GetConfig().get("provision.primary_ecu_hardware_id", "")
              << ", tag: " << client.GetConfig().get("pacman.tags", "");
    return StatusCode::TufTargetNotFound;
  }

  // Run the target installation
  LOG_INFO << "Found Target: " << target.Name();
  const auto installer = client.Installer(target);

  auto dr = installer->Download();
  if (!dr) {
    LOG_ERROR << "Failed to download Target; target: " << target.Name() << ", err: " << dr;
    return res2StatusCode<DownloadResult::Status>(d2s, dr.status);
  }

  auto ir = installer->Install();
  if (!ir) {
    LOG_ERROR << "Failed to install Target; target: " << target.Name() << ", err: " << ir;
    if (ir.status == InstallResult::Status::Failed) {
      LOG_INFO << "Rolling back to the previous target: " << current.Name() << "...";
      const auto installer = client.Installer(current);
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

StatusCode CompleteInstall(AkliteClient &client) {
  if (!client.IsInstallationInProgress()) {
    LOG_ERROR << "There is no pending installation to complete";
    return StatusCode::NoPendingInstallation;
  }
  const auto pending{client.GetPendingTarget()};  // returns Target that a device was supposed to boot on
  const auto ir = client.CompleteInstallation();
  if (!ir) {
    LOG_ERROR << "Failed to finalize pending installation; target: " << pending.Name() << ", err: " << ir;

    // check rollback type, the bootloader or App driven
    const auto current{client.GetCurrent()};        // returns Target a device is booted on
    const auto pending{client.GetPendingTarget()};  // returns Target that a device was supposed to boot on
    if (current.Sha256Hash() != pending.Sha256Hash()) {
      // ostree rollback, aka the bootloader driven rollback
      auto ri = client.CheckAppsInSync();
      const auto rir = ri->Install();
      if (rir.status == InstallResult::Status::Ok) {
        return StatusCode::InstallRollbackOk;
      } else {
        return StatusCode::InstallRollbackFailed;
      }
    } else {
      // TODO
      return StatusCode::InstallRollbackFailed;
    }
  }

  return res2StatusCode<InstallResult::Status>(i2s, ir.status);
}

}  // namespace cli
