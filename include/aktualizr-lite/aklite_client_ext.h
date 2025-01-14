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
    // Regular error situations
    TufTargetNotFound = 100,
    TargetAlreadyInstalled,
    RollbackTargetNotFound,

    // Internal errors
    BadRollbackTarget = 110,
    BadCheckinStatus,

    // Success results
    NoUpdate = 120,
    UpdateNewVersion,
    UpdateSyncApps,
    UpdateRollback,
  };

  explicit GetTargetToInstallResult(const CheckInResult &checkin_res)
      : status(static_cast<Status>(checkin_res.status)) {}

  GetTargetToInstallResult(Status status, TufTarget selected_target, std::string reason)
      : status(status), selected_target(std::move(selected_target)), reason(std::move(reason)) {}
  Status status;

  // NOLINTNEXTLINE(hicpp-explicit-conversions,google-explicit-constructor)
  operator bool() const {
    return status == Status::NoUpdate || status == Status::UpdateNewVersion || status == Status::UpdateSyncApps ||
           status == Status::UpdateRollback;
  }

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
  explicit AkliteClientExt(std::shared_ptr<LiteClient> client, bool read_only = false, bool apply_lock = false,
                           bool invoke_post_cb_at_checkin = true)
      : AkliteClient(std::move(client), read_only, apply_lock) {
    invoke_post_cb_at_checkin_ = invoke_post_cb_at_checkin;
  }

  explicit AkliteClientExt(const boost::program_options::variables_map &cmdline_args, bool read_only = false,
                           bool finalize = true, bool invoke_post_cb_at_checkin = true)
      : AkliteClient(cmdline_args, read_only, finalize) {
    invoke_post_cb_at_checkin_ = invoke_post_cb_at_checkin;
  }

  GetTargetToInstallResult GetTargetToInstall(const CheckInResult &checkin_res, int version = -1,
                                              const std::string &target_name = "", bool allow_bad_target = false,
                                              bool force_apps_sync = false, bool offline_mode = false);
  InstallResult PullAndInstall(const TufTarget &target, const std::string &reason = "",
                               const std::string &correlation_id = "", InstallMode install_mode = InstallMode::All,
                               const LocalUpdateSource *local_update_source = nullptr, bool do_download = true,
                               bool do_install = true, bool require_target_in_tuf = true);

  bool RebootIfRequired();

 private:
  bool cleanup_removed_apps_{true};
};

#endif
