#ifndef AKTUALIZR_LITE_CLI_H_
#define AKTUALIZR_LITE_CLI_H_

#include <cstdlib>
#include <string>

class AkliteClient;

namespace aklite::cli {

enum class StatusCode {
  UnknownError = EXIT_FAILURE,
  Ok = EXIT_SUCCESS,
  CheckinOkCached = 3,
  CheckinFailure = 4,
  OkNeedsRebootForBootFw = 5,
  TufMetaPullFailure = 10,
  TufTargetNotFound = 20,
  InstallationInProgress = 30,
  NoPendingInstallation = 40,
  DownloadFailure = 50,
  DownloadFailureNoSpace = 60,
  DownloadFailureVerificationFailed = 70,
  InstallAppPullFailure = 80,
  InstallNeedsRebootForBootFw = 90,
  InstallNeedsReboot = 100,
  InstallAppsNeedFinalization = 105,
  InstallRollbackOk = 110,
  InstallRollbackNeedsReboot = 120,
  InstallRollbackFailed = 130,
};

StatusCode CheckLocal(AkliteClient &client, const std::string &tuf_repo, const std::string &ostree_repo,
                      const std::string &apps_dir = "");

StatusCode Install(AkliteClient &client, int version = -1, const std::string &target_name = "",
                   const std::string &install_mode = "");
StatusCode CompleteInstall(AkliteClient &client);

}  // namespace aklite::cli

#endif  // AKTUALIZR_LITE_CLI_H_
