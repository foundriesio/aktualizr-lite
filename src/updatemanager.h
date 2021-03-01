#ifndef AKTUALIZR_LITE_UPDATE_META_H_
#define AKTUALIZR_LITE_UPDATE_META_H_

#include <boost/optional.hpp>

#include "uptane/tuf.h"

struct UpdateMeta {
  enum UpdateType { kNone = 0, kInstall, kSync, kSyncRemove, kUpdate };

  Uptane::Target from_target;            // current, not shortlisted Target, needed for logger
  Uptane::Target to_target;              // Target to update to, not shortlisted Target, goes to DB and logger
  Uptane::Target shortlisted_to_target;  // Target to update to, shortlisted, needed for prunning
  Uptane::Target target_to_apply;  // Target to apply, shortlisted and removed currently installed and running apps.
  std::string update_reason;       // Report to the backend
  UpdateType update_type;
};

class UpdateManager {
 public:
  UpdateManager(boost::optional<std::set<std::string>> app_shortlist) : app_shortlist_{app_shortlist} {}

  UpdateMeta initUpdate(const Uptane::Target& current, const Uptane::Target& from_target,
                        const Uptane::Target& to_target);

  void logUpdate(const UpdateMeta& update) const;

 private:
  boost::optional<std::set<std::string>> app_shortlist_;
};

#endif  // AKTUALIZR_LITE_UPDATE_META_H_
