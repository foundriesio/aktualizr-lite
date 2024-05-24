#ifndef AKTUALIZR_LITE_CLI_H_
#define AKTUALIZR_LITE_CLI_H_

#include <cstdlib>
#include <string>

#include "aktualizr-lite/aklite_client_ext.h"
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
  CheckinSecurityError = 11,
  CheckinExpiredMetadata = 12,
  CheckinMetadataFetchFailure = 13,
  CheckinMetadataNotFound = 14,
  CheckinInvalidBundleMetadata = 15,
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

/**
 * The installation mode to be applied. Specified during InstallContext context initialization.
 */
enum class PullMode {
  /**
   * Default mode, do pull target during install operation.
   */
  All = 0,
  /**
   * Do no pull target during install. Target is expected to be pulled before.
   */
  None,
};

StatusCode CheckIn(AkliteClient &client, const LocalUpdateSource *local_update_source);

StatusCode Pull(AkliteClientExt &client, int version = -1, const std::string &target_name = "",
                bool force_downgrade = true, const LocalUpdateSource *local_update_source = nullptr);

StatusCode Install(AkliteClientExt &client, int version = -1, const std::string &target_name = "",
                   InstallMode install_mode = InstallMode::All, bool force_downgrade = true,
                   const LocalUpdateSource *local_update_source = nullptr, PullMode pull_mode = PullMode::All);

StatusCode CompleteInstall(AkliteClient &client);

bool IsSuccessCode(StatusCode status);

}  // namespace aklite::cli

#endif  // AKTUALIZR_LITE_CLI_H_
