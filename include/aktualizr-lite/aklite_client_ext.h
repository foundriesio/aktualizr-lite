#ifndef AKTUALIZR_LITE_CLIENT_EXT_H_
#define AKTUALIZR_LITE_CLIENT_EXT_H_

#include <cstdlib>
#include <string>

#include "json/json.h"

#include "aktualizr-lite/tuf/tuf.h"

#include "aktualizr-lite/api.h"

class GetTargetToInstallResult {
 public:
  //
  enum class Status {
    // First block must match CheckInResult::Status
    Ok = static_cast<int>(CheckInResult::Status::Ok),  // check-in was good
    OkCached,                                          // check-in failed, but locally cached meta-data is still valid
    Failed,                                            // check-in failed and there's no valid local meta-data
    NoMatchingTargets,
    NoTargetContent,
    SecurityError,
    ExpiredMetadata,
    MetadataFetchFailure,
    MetadataNotFound,
    BundleMetadataError,

    // Additional values, specific for GetTargetToInstallResult
    TufTargetNotFound = 100,
    TargetAlreadyInstalled,
  };

  explicit GetTargetToInstallResult(const CheckInResult &check_in_result)
      : status(static_cast<Status>(check_in_result.status)) {}

  GetTargetToInstallResult(Status status, TufTarget selected_target, std::string reason)
      : status(status), selected_target(std::move(selected_target)), reason(std::move(reason)) {}
  Status status;

  // NOLINTNEXTLINE(hicpp-explicit-conversions,google-explicit-constructor)
  operator bool() const { return status == Status::Ok || status == Status::OkCached; }

  TufTarget selected_target;
  std::string reason;
};

/**
 * @brief contains additional methods that consolidate functionality
 * making it reusable between the main aklite daemon and other tools.
 *
 * Those methods may eventually be part of the supported API but, for now,
 * they are likely to have they signature and behavior changed.
 */
class AkliteClientExt : public AkliteClient {
  // Inherit constructors
  using AkliteClient::AkliteClient;

  struct NoSpaceDownloadState {
    std::string ostree_commit_hash;
    std::string cor_id;
    storage::Volume::UsageInfo stat;
  } state_when_download_failed{"", "", {.err = "undefined"}};

 public:
  GetTargetToInstallResult GetTargetToInstall(const LocalUpdateSource *local_update_source = nullptr, int version = -1,
                                              const std::string &target_name = "", bool allow_bad_target = false,
                                              bool force_apps_sync = false);
  InstallResult PullAndInstall(const TufTarget &target, const std::string &reason = "",
                               const std::string &correlation_id = "", InstallMode install_mode = InstallMode::All,
                               const LocalUpdateSource *local_update_source = nullptr, bool do_download = true,
                               bool do_install = true);

  bool RebootIfRequired();
};

#endif
