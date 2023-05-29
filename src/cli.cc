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

  const auto targets = client.CheckIn();
  if (version == -1) {
    target = targets.GetLatest();
  } else {
    for (const auto& t : targets.Targets()) {
      if (t.Version() == version) {
        target = t;
        break;
      }
    }
  }

  LOG_INFO << "Found Target: " << target.Name();
  auto installer = client.Installer(target, "", "");

  auto donwload_res = installer->Download();
  switch (donwload_res.status) {
    case DownloadResult::Status::DownloadFailed: {
      return ExitCode::DownloadFailure;
      break;
    }
    default:
      // TODO
      break;
  }

  auto install_res = installer->Install();
  switch (install_res.status) {
    case InstallResult::Status::Ok: {
      exit_code = ExitCode::Ok;
      break;
    }
    case InstallResult::Status::NeedsCompletion: {
      exit_code = ExitCode::InstallNeedsReboot;
      break;
    }
    default:
      // TODO
      break;
  }

  // TODO
  return exit_code;
}

ExitCode CompleteInstall(AkliteClient& client) {
  TufTarget target{client.GetPendingTarget()};

  if (target.IsUnknown()) {
    LOG_INFO << "There is no pending installation to complete";
    return ExitCode::NoPendingInstallation;
  }

  if (client.CompleteInstallation()) {
    return ExitCode::Ok;
  }

  // TODO
  return ExitCode::UnknownError;
}

}  // namespace cli
