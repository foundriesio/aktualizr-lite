#include "updatemanager.h"
#include "target.h"

UpdateMeta UpdateManager::initUpdate(const Uptane::Target& current, const Uptane::Target& from_target,
                                     const Uptane::Target& to_target) {
  auto update_target_apps = Target::Apps(to_target);
  auto shortlisted_to_target_apps = Target::Apps(to_target);
  auto currently_installed_apps = Target::Apps(current);
  auto current_target_apps = Target::Apps(from_target);
  bool are_apps_to_remove{false};

  for (const auto& app : Target::Apps(to_target)) {
    if (!!app_shortlist_ && 0 == app_shortlist_->count(app.name)) {
      // if there is an app shortlist and App is not included into it then remove it from targets
      shortlisted_to_target_apps.remove(app);
      update_target_apps.remove(app);
      if (currently_installed_apps.exists(app)) {
        LOG_INFO << ">>>> " << app.name << " will be removed";
        are_apps_to_remove = true;
      }
    } else if (currently_installed_apps.exists(app)) {
      // if there is no an app shortlist or App is listed in the shortlist make sure that it's not already
      // installed&running otherwise, remove it from the update
      update_target_apps.remove(app);
    } else {
      if (current_target_apps.exists(app)) {
        LOG_INFO << ">>>> " << app.name << " will be re-installed";
      } else {
        LOG_INFO << ">>>> " << app.name << " will be updated: ";
      }
    }
  }

  Uptane::Target shortlisted_to_target{shortlisted_to_target_apps.createTarget(to_target)};
  Uptane::Target update_target{update_target_apps.createTarget(to_target)};

  std::string update_reason;
  UpdateMeta::UpdateType update_type{UpdateMeta::UpdateType::kNone};

  if (update_target.sha256Hash() == current.sha256Hash() && Target::Apps(update_target).empty() &&
      !are_apps_to_remove) {
    return {from_target, to_target, shortlisted_to_target, update_target, update_reason, update_type};
  }

  if (!from_target.IsValid()) {
    update_reason = "Update to " + to_target.filename();
    update_type = UpdateMeta::UpdateType::kInstall;
  } else if (to_target.filename() != from_target.filename()) {
    update_reason = "Update from " + from_target.filename() + " to " + to_target.filename();
    update_type = UpdateMeta::UpdateType::kUpdate;
  } else {
    update_reason = "Syncing current Target: " + from_target.filename();

    if (Target::Apps(update_target).empty() && are_apps_to_remove) {
      update_type = UpdateMeta::UpdateType::kSyncRemove;
    } else {
      update_type = UpdateMeta::UpdateType::kSync;
    }
  }

  Target::setCorrelationID(update_target);
  return {from_target, to_target, shortlisted_to_target, update_target, update_reason, update_type};
}

void UpdateManager::logUpdate(const UpdateMeta& update) const {
  switch (update.update_type) {
    case UpdateMeta::UpdateType::kNone: {
      LOG_INFO << "Active Target is in sync with the specified Target: " << update.to_target.filename();
      break;
    }
    case UpdateMeta::UpdateType::kInstall: {
      Target::log("Updating to Target: ", update.to_target, app_shortlist_);
      break;
    }
    case UpdateMeta::UpdateType::kSync:
    case UpdateMeta::UpdateType::kSyncRemove: {
      Target::log("Syncing current Target: ", update.from_target, app_shortlist_);
      break;
    }
    case UpdateMeta::UpdateType::kUpdate: {
      Target::log("Updating Active Target: ", update.from_target, app_shortlist_);
      Target::log("To New Target: ", update.to_target, app_shortlist_);
      break;
    }
    default: {
      LOG_ERROR << "Invalid update type: " << update.update_type;
    }
  }  // switch
}
