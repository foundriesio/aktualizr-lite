#include "updatemanager.h"
#include "target.h"

UpdateMeta UpdateManager::initUpdate(const Uptane::Target& current, const Uptane::Target& from_target,
                                     const Uptane::Target& to_target) {
  Uptane::Target shortlisted_to_target = to_target;
  Uptane::Target update_target = to_target;

  auto update_target_custom = update_target.custom_data();
  auto shortlisted_to_target_custom = shortlisted_to_target.custom_data();
  auto current_apps = current.custom_data().get(Target::ComposeAppField, Json::nullValue);

  for (const auto& app : Target::Apps(to_target)) {
    if (!!app_shortlist_ && 0 == app_shortlist_->count(app.name)) {
      // if there is an app shortlist and App is not included into it then remove it from targets
      shortlisted_to_target_custom[Target::ComposeAppField].removeMember(app.name);
      update_target_custom[Target::ComposeAppField].removeMember(app.name);
    } else if (current_apps.isMember(app.name)) {
      // if there is no an app shortlist or App is listed in the shortlist make sure that it's not already
      // installed&running otherwise, remove it from the update
      update_target_custom[Target::ComposeAppField].removeMember(app.name);
    } else {
      // Determine if there is actually need to do any update at all, e.g. no new Target and everything is in sync
      // TODO: log what will be fetched & installed
      LOG_INFO << ">>>>>>>> " << app.name << " will be installed";
    }
  }

  update_target.updateCustom(update_target_custom);
  shortlisted_to_target.updateCustom(shortlisted_to_target_custom);

  if (update_target.sha256Hash() == current.sha256Hash() &&
      update_target.custom_data()[Target::ComposeAppField].empty()) {
    LOG_ERROR << "Nothing to update";
  }

  std::string update_reason;
  if (!from_target.IsValid()) {
    update_reason = "Update to " + to_target.filename();

    Target::log("Updating to Target: ", to_target, app_shortlist_);

  } else if (to_target.filename() != from_target.filename()) {
    update_reason = "Update from " + from_target.filename() + " to " + to_target.filename();

    Target::log("Updating Active Target: ", from_target, app_shortlist_);
    Target::log("To New Target: ", to_target, app_shortlist_);

  } else {
    Target::log("Syncing current Target: ", from_target, app_shortlist_);
    update_reason = "Syncing current Target: " + from_target.filename();
  }

  Target::setCorrelationID(update_target);

  return {to_target, shortlisted_to_target, update_target, update_reason};
}
