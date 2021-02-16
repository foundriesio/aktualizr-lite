#include "target.h"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

bool Target::hasTag(const Uptane::Target& target, const std::vector<std::string>& tags) {
  if (tags.empty()) {
    return true;
  }

  const auto target_tags = target.custom_data()[TagField];
  for (Json::ValueConstIterator ii = target_tags.begin(); ii != target_tags.end(); ++ii) {
    auto target_tag = (*ii).asString();
    if (std::find(tags.begin(), tags.end(), target_tag) != tags.end()) {
      return true;
    }
  }
  return false;
}

void Target::setCorrelationID(Uptane::Target& target) {
  std::string id = target.custom_version();
  if (id.empty()) {
    id = target.filename();
  }
  boost::uuids::uuid tmp = boost::uuids::random_generator()();
  target.setCorrelationId(id + "-" + boost::uuids::to_string(tmp));
}
