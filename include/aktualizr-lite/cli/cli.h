#ifndef AKTUALIZR_LITE_CLI_H_
#define AKTUALIZR_LITE_CLI_H_

#include <cstdlib>
#include <string>

#include "aktualizr-lite/api.h"

namespace aklite::cli {

enum class StatusCode {
  UnknownError = EXIT_FAILURE,
  Ok = EXIT_SUCCESS,
  CheckinOkCached = 3,
  CheckinFailure = 4,
  OkNeedsRebootForBootFw = 5,
  CheckinNoMatchingTargets = 6,
  CheckinNoTargetContent = 8,
  InstallAppsNeedFinalization = 10,
  TufTargetNotFound = 20,
  InstallationInProgress = 30,
  NoPendingInstallation = 40,
  DownloadFailure = 50,
  DownloadFailureNoSpace = 60,
  DownloadFailureVerificationFailed = 70,
  InstallAlreadyInstalled = 75,
  InstallAppPullFailure = 80,
  InstallNeedsRebootForBootFw = 90,
  InstallOfflineRollbackOk = 99,
  InstallNeedsReboot = 100,
  InstallDowngradeAttempt = 102,
  InstallRollbackOk = 110,
  InstallRollbackNeedsReboot = 120,
  InstallRollbackFailed = 130,
};

StatusCode CheckLocal(AkliteClient &client, const std::string &tuf_repo, const std::string &ostree_repo,
                      const std::string &apps_dir = "");

StatusCode Install(AkliteClient &client, int version = -1, const std::string &target_name = "",
                   InstallMode install_mode = InstallMode::All, bool force_downgrade = true,
                   const LocalUpdateSource *local_update_source = nullptr);
StatusCode CompleteInstall(AkliteClient &client);

}  // namespace aklite::cli

#endif  // AKTUALIZR_LITE_CLI_H_
