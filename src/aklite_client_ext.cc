#include "aktualizr-lite/aklite_client_ext.h"

#include <sys/file.h>
#include <unistd.h>
#include <boost/format.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "aktualizr-lite/api.h"
#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "liteclient.h"
#include "logging/logging.h"
#include "primary/reportqueue.h"

#include "aktualizr-lite/tuf/tuf.h"
#include "composeapp/appengine.h"
#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "ostree/repo.h"
#include "tuf/akhttpsreposource.h"
#include "tuf/akrepo.h"
#include "tuf/localreposource.h"
#include "uptane/exceptions.h"

// Returns the target that should be installed, if any.
// It might be an updated version, a rollback target, or even the currently installed target, in case we need to sync
// apps
GetTargetToInstallResult AkliteClientExt::GetTargetToInstall(const CheckInResult& checkin_res, int version,
                                                             const std::string& target_name, bool allow_bad_target,
                                                             bool force_apps_sync, bool is_offline_mode,
                                                             bool auto_downgrade) {
  client_->setAppsNotChecked();

  std::string err;
  if (!checkin_res) {
    err = "Can't select target to install using a failed check-in result";
    LOG_WARNING << err << " " << static_cast<int>(checkin_res.status);
    if (!invoke_post_cb_at_checkin_) {
      client_->notifyTufUpdateFinished(err);
    }
    return {GetTargetToInstallResult::Status::BadCheckinStatus, TufTarget(), err};
  }

  bool rollback_operation = false;
  auto candidate_target = checkin_res.SelectTarget(version, target_name);
  if (candidate_target.IsUnknown()) {
    err = "No matching target";
    if (!invoke_post_cb_at_checkin_) {
      client_->notifyTufUpdateFinished(err);
    }
    LOG_WARNING << err;
    return {GetTargetToInstallResult::Status::TufTargetNotFound, TufTarget(), err};
  }

  const auto current = GetCurrent();
  // It may occur that the TUF targets list only has versions lower than the current one.
  // The `auto_downgrade` parameter controls what to do in such situation: Should a version lower than the current
  //  one be accepted as a valid selected target for installation or not
  if (!auto_downgrade && version == -1 && target_name.empty() && candidate_target.Version() < current.Version()) {
    if (!invoke_post_cb_at_checkin_) {
      client_->notifyTufUpdateFinished(err);
    }
    LOG_INFO << "Rejecting latest target in TUF metadata to prevent downgrade. Current: " << current.Version()
             << " candidate: " << candidate_target.Version();
    return {GetTargetToInstallResult::Status::NoUpdate, TufTarget(), ""};
  }

  if (IsRollback(current) && current.Name() == candidate_target.Name()) {
    // Handle the case when Apps failed to start on boot just after an update.
    // This is only possible with `pacman.create_containers_before_reboot = 0`.
    LOG_INFO << "The currently booted Target is a failing Target, finding Target to rollback to...";
    const TufTarget rollback_target = Target::toTufTarget(client_->getRollbackTarget());
    if (rollback_target.IsUnknown()) {
      const auto err{boost::str(boost::format("Failed to find Target to rollback to after a failure to start "
                                              "Apps at boot on a new version of sysroot;"
                                              " failing current Target: %s, hash: %s") %
                                current.Name() % current.Sha256Hash())};

      LOG_ERROR << err;
      if (!invoke_post_cb_at_checkin_) {
        client_->notifyTufUpdateFinished(err);
      }
      return {GetTargetToInstallResult::Status::RollbackTargetNotFound, TufTarget(), err};
    }
    candidate_target = rollback_target;
    rollback_operation = true;
    LOG_INFO << "Found Target to rollback to: " << rollback_target.Name() << ", hash: " << rollback_target.Sha256Hash();
  }

  // This is a workaround for finding and avoiding bad updates after a rollback.
  // Rollback sets the installed version state to none instead of broken, so there is no
  // easy way to find just the bad versions without api/storage changes. As a workaround we
  // just check if the version is not current nor pending nor known (old hash) and never been successfully
  // installed, if so then skip an update to the such version/Target
  auto is_bad_target = IsRollback(candidate_target);
  // Extra state validation
  if (rollback_operation && is_bad_target) {
    // We should never get here: when a rollback initiated required, a bad target should never be selected
    const auto err{
        boost::str(boost::format("A bad target (%s) was selected for rollback of %s. This should not happen") %
                   candidate_target.Name() % current.Name())};
    LOG_ERROR << err;
    if (!invoke_post_cb_at_checkin_) {
      client_->notifyTufUpdateFinished(err);
    }
    return {GetTargetToInstallResult::Status::BadRollbackTarget, TufTarget(), err};
  }

  auto res = GetTargetToInstallResult(GetTargetToInstallResult::Status::NoUpdate, candidate_target, "");
  if (candidate_target.Name() != current.Name() && (!is_bad_target || allow_bad_target)) {
    if (!rollback_operation && !is_bad_target) {
      LOG_INFO << "Found new and valid Target to update to: " << candidate_target.Name()
               << ", sha256: " << candidate_target.Sha256Hash();
      LOG_INFO << "Updating Active Target: " << current.Name();
      LOG_INFO << "To New Target: " << candidate_target.Name();
    } else if (is_bad_target) {
      // force is true at this point
      LOG_INFO << candidate_target.Name()
               << " target is marked for causing a rollback, but installation will be forced ";
    }
    // We should install this target:
    res.status = rollback_operation ? GetTargetToInstallResult::Status::UpdateRollback
                                    : GetTargetToInstallResult::Status::UpdateNewVersion;
    res.selected_target = candidate_target;
    res.reason = std::string(rollback_operation ? "Rolling back" : "Updating") + " from " + current.Name() + " to " +
                 res.selected_target.Name();

  } else {
    if (is_bad_target) {
      LOG_INFO << "Target: " << candidate_target.Name() << " is a failing Target (aka known locally)."
               << " Skipping its installation.";
    }

    auto apps_to_update = client_->appsToUpdate(Target::fromTufTarget(current), cleanup_removed_apps_);
    // Automatically cleanup during check only once. A cleanup will also occur after a new target is installed
    cleanup_removed_apps_ = false;
    if (force_apps_sync || !apps_to_update.empty()) {
      // Force installation of apps
      res.selected_target = checkin_res.SelectTarget(current.Version());
      if (res.selected_target.IsUnknown()) {
        LOG_DEBUG << "Unable to find current version " << current.Version()
                  << " in TUF targets list. Using current target from DB instead";
        res.selected_target = current;
      }
      LOG_INFO
          << "The specified Target is already installed, enforcing installation to make sure it's synced and running:"
          << res.selected_target.Name();

      res.status = GetTargetToInstallResult::Status::UpdateSyncApps;
      res.reason = "Syncing Active Target Apps\n";
      for (const auto& app_to_update : apps_to_update) {
        res.reason += "- " + app_to_update.first + ": " + app_to_update.second + "\n";
      }
    } else {
      // No targets to install
      res.selected_target = TufTarget();
      if (!is_offline_mode) {
        // Online mode
        LOG_INFO << "Device is up-to-date";
      } else {
        // Offline mode
        LOG_INFO << "Target " << candidate_target.Name() << " is already installed";
        res.status = GetTargetToInstallResult::Status::TargetAlreadyInstalled;
      }
    }
    client_->setAppsNotChecked();
  }

  if (!invoke_post_cb_at_checkin_) {
    client_->notifyTufUpdateFinished("", Target::fromTufTarget(candidate_target));
  }

  return res;
}

static const std::unordered_map<DownloadResult::Status, InstallResult::Status> dr2ir = {
    {DownloadResult::Status::Ok, InstallResult::Status::Ok},
    {DownloadResult::Status::DownloadFailed, InstallResult::Status::DownloadOstreeFailed},
    {DownloadResult::Status::VerificationFailed, InstallResult::Status::VerificationFailed},
    {DownloadResult::Status::DownloadFailed_NoSpace, InstallResult::Status::DownloadFailed_NoSpace},
};

InstallResult AkliteClientExt::PullAndInstall(const TufTarget& target, const std::string& reason,
                                              const std::string& correlation_id, const InstallMode install_mode,
                                              const LocalUpdateSource* local_update_source, const bool do_download,
                                              const bool do_install, bool require_target_in_tuf) {
  // Check if the device is in a correct state to start a new update
  if (IsInstallationInProgress()) {
    LOG_ERROR << "Cannot start Target installation since there is ongoing installation; target: "
              << GetPendingTarget().Name();
    return InstallResult{InstallResult::Status::InstallationInProgress};
  }

  const auto current{GetCurrent()};

  // Prior to performing the update, check if aklite haven't tried to fetch the target ostree before,
  // and it failed due to lack of space, and the space has not increased since that time.
  if (state_when_download_failed.stat.required.first > 0 && state_when_download_failed.stat.isOk() &&
      target.Sha256Hash() == state_when_download_failed.ostree_commit_hash) {
    storage::Volume::UsageInfo current_usage_info{storage::Volume::getUsageInfo(
        state_when_download_failed.stat.path, static_cast<int>(state_when_download_failed.stat.reserved.second),
        state_when_download_failed.stat.reserved_by)};
    if (!current_usage_info.isOk()) {
      LOG_ERROR << "Failed to obtain storage usage statistic: " << current_usage_info.err;
    }

    if (current_usage_info.isOk() &&
        current_usage_info.available.first < state_when_download_failed.stat.required.first) {
      const std::string err_msg{"Insufficient storage available at " + state_when_download_failed.stat.path +
                                " to download Target: " + target.Name() + ", " +
                                current_usage_info.withRequired(state_when_download_failed.stat.required.first).str() +
                                " (cached status)"};
      LOG_ERROR << err_msg;
      auto event_target = Target::fromTufTarget(target);
      event_target.setCorrelationId(state_when_download_failed.cor_id);
      client_->notifyDownloadFinished(event_target, false, err_msg);
      return InstallResult{InstallResult::Status::DownloadFailed_NoSpace, err_msg};
    }
  }
  state_when_download_failed = {"", "", {.err = "undefined"}};

  const auto installer =
      Installer(target, reason, correlation_id, install_mode, local_update_source, require_target_in_tuf);
  if (installer == nullptr) {
    LOG_ERROR << "Unexpected error: installer couldn't find Target in the DB; try again later";
    return InstallResult{InstallResult::Status::UnknownError};
  }

  if (do_download) {
    auto dr = installer->Download();
    if (!dr) {
      if (dr.noSpace()) {
        state_when_download_failed = {target.Sha256Hash(), correlation_id, dr.stat};
      }

      LOG_ERROR << "Failed to download Target; target: " << target.Name() << ", err: " << dr;
      return InstallResult{dr2ir.at(dr.status), dr.description};
    }

    if (!do_install) {
      return InstallResult{dr2ir.at(dr.status), dr.description};
    }
  }

  auto ir = installer->Install();
  if (!ir) {
    LOG_ERROR << "Failed to install Target; target: " << target.Name() << ", err: " << ir;
    if (ir.status == InstallResult::Status::Failed) {
      LOG_INFO << "Rolling back to the previous target: " << current.Name() << "...";
      const auto installer =
          Installer(current, ir.description, correlation_id, InstallMode::All, local_update_source, false);
      if (installer == nullptr) {
        LOG_ERROR << "Failed to find the previous target in the TUF Targets DB";
        return InstallResult{InstallResult::Status::InstallRollbackFailed, ir.description};
      }
      ir = installer->Install();
      if (!ir) {
        LOG_ERROR << "Failed to rollback to " << current.Name() << ", err: " << ir;
      }
      if (ir.status == InstallResult::Status::Ok) {
        return InstallResult{InstallResult::Status::InstallRollbackOk, ir.description};
      } else {
        return InstallResult{InstallResult::Status::InstallRollbackFailed, ir.description};
      }
    }
  }

  return ir;
}

bool AkliteClientExt::RebootIfRequired() {
  auto is_reboot_required = client_->isRebootRequired();

  if (is_reboot_required.first) {
    if (!is_booted_env) {
      LOG_WARNING << "Skipping reboot operation because this is not a booted environment";
    } else if (is_reboot_required.second.empty()) {
      LOG_WARNING << "Skipping reboot operation since reboot command is not set";
    } else {
      LOG_INFO << "Device is going to reboot (" << is_reboot_required.second << ")";
      if (setuid(0) != 0) {
        LOG_ERROR << "Failed to set/verify a root user so cannot reboot system programmatically";
      } else {
        sync();
        // let's try to reboot the system, if it fails we just throw an exception and exit the process
        if (std::system(is_reboot_required.second.c_str()) != 0) {
          LOG_ERROR << "Failed to execute the reboot command: " + is_reboot_required.second;
        }
      }
    }
    // Return true means we are supposed to stop execution
    return true;
  }

  return false;
}

InstallResult AkliteClientExt::Rollback(const LocalUpdateSource* local_update_source) {
  const auto current = GetCurrent();
  // Getting Uptane::Target instance in order to use correlation_id, which is not available in TufTarget class
  auto pending_target = client_->getPendingTarget();
  auto installation_in_progress = pending_target.IsValid();
  const auto& bad_target = installation_in_progress ? Target::toTufTarget(pending_target) : current;

  LOG_DEBUG << "User initiated rollback. Current Target is " << current.Name();
  if (installation_in_progress) {
    LOG_DEBUG << "Target installation is in progress:  " << pending_target.filename();
  }

  auto storage = INvStorage::newStorage(client_->config.storage, false);
  LOG_INFO << "Marking target " << bad_target.Name() << " as a failing target";
  storage->saveInstalledVersion("", Target::fromTufTarget(bad_target), InstalledVersionUpdateMode::kBadTarget);

  // Get rollback target
  auto rollback_target = GetRollbackTarget(installation_in_progress);
  if (rollback_target.IsUnknown()) {
    LOG_ERROR << "Failed to find Target to rollback to";
    return {InstallResult::Status::Failed};
  }

  if (installation_in_progress) {
    // Previous installation was not finalized
    LOG_INFO << "Creating new installation log entry for " << pending_target.filename()
             << ", as we try to rollback to it";
    storage->saveInstalledVersion("", pending_target, InstalledVersionUpdateMode::kNone);
  }

  std::string reason = "User initiated rollback. Marked " + bad_target.Name() +
                       " as a failing target, and rolling back to " + rollback_target.Name();
  LOG_INFO << reason;

  if (installation_in_progress) {
    LOG_INFO << "Generating installation failed event / callback for Target " << pending_target.filename();
    data::InstallationResult result(data::ResultCode::Numeric::kInstallFailed, reason);
    client_->notifyInstallFinished(pending_target, result);
  }

  // If there is an installation in progress, do not perform download, and don't require target to be in TUF targets
  auto ret = PullAndInstall(rollback_target, reason, "", InstallMode::All, local_update_source,
                            !installation_in_progress, true, !installation_in_progress);
  return ret;
}

bool AkliteClientExt::IsAppRunning(const std::string& name, const std::string& uri) const {
  return client_->isAppRunning({name, uri});
};
