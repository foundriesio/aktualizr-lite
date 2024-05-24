#include "aktualizr-lite/aklite_client_ext.h"

#include <sys/file.h>
#include <unistd.h>
#include <boost/format.hpp>
#include <boost/process.hpp>
#include <boost/property_tree/ini_parser.hpp>
#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "liteclient.h"
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
GetTargetToInstallResult AkliteClientExt::GetTargetToInstall(const LocalUpdateSource* local_update_source, int version,
                                                             const std::string& target_name, bool allow_bad_target,
                                                             bool force_apps_sync) {
  // Tell CheckIn and CheckInLocal methods to not call notifyTufUpdateFinished in case of success
  usingUpdateClientApi = true;
  client_->setAppsNotChecked();
  bool rollback_operation = false;

  CheckInResult checkin_res = local_update_source == nullptr ? CheckIn() : CheckInLocal(local_update_source);

  if (!checkin_res) {
    LOG_WARNING << "Unable to update latest metadata";
    return GetTargetToInstallResult(checkin_res);
  }

  auto candidate_target = checkin_res.SelectTarget(version, target_name);
  if (candidate_target.IsUnknown()) {
    std::string err = "No matching target";
    client_->notifyTufUpdateFinished(err);
    LOG_WARNING << err;
    return {GetTargetToInstallResult::Status::TufTargetNotFound, TufTarget(), err};
  }

  const auto current = GetCurrent();
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
      client_->notifyTufUpdateFinished(err);
      // TODO: Specify proper error status
      return {GetTargetToInstallResult::Status::Failed, TufTarget(), err};
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
    client_->notifyTufUpdateFinished(err);
    return {GetTargetToInstallResult::Status::Failed, TufTarget(), err};
  }

  auto res = GetTargetToInstallResult(GetTargetToInstallResult::Status::Ok, candidate_target, "");
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
    res.selected_target = candidate_target;
    res.reason = std::string(rollback_operation ? "Rolling back" : "Updating") + " from " + current.Name() + " to " +
                 res.selected_target.Name();

  } else {
    if (is_bad_target) {
      LOG_INFO << "Target: " << candidate_target.Name() << " is a failing Target (aka known locally)."
               << " Skipping its installation.";
    }

    if (force_apps_sync || !client_->appsInSync(Target::fromTufTarget(current))) {
      // Force installation of apps
      res.selected_target = current;
      LOG_INFO
          << "The specified Target is already installed, enforcing installation to make sure it's synced and running:"
          << res.selected_target.Name();

      res.reason = "Syncing Active Target Apps";
    } else {
      // No targets to install
      res.selected_target = TufTarget();
      if (local_update_source == nullptr) {
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

  client_->notifyTufUpdateFinished("", Target::fromTufTarget(candidate_target));

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
                                              const bool do_install) {
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

  const auto installer = Installer(target, reason, correlation_id, install_mode, local_update_source);
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
      const auto installer = Installer(current);
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
    if (is_reboot_required.second.empty()) {
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
