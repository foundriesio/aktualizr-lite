#ifndef AKTUALIZR_LITE_UPDATE_META_H_
#define AKTUALIZR_LITE_UPDATE_META_H_

#include <boost/optional.hpp>

#include "uptane/tuf.h"

struct UpdateMeta {
  Uptane::Target to_target;              // goes to DB and logger
  Uptane::Target shortlisted_to_target;  // need for prunning
  Uptane::Target target_to_apply;        // what to install
  std::string update_reason;
};

class UpdateManager {
 public:
  UpdateManager(boost::optional<std::set<std::string>> app_shortlist) : app_shortlist_{app_shortlist} {}

  UpdateMeta initUpdate(const Uptane::Target& current, const Uptane::Target& from_target,
                        const Uptane::Target& to_target);

 private:
  boost::optional<std::set<std::string>> app_shortlist_;
};

#endif  // AKTUALIZR_LITE_UPDATE_META_H_
