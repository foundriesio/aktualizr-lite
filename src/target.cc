#include "target.h"

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
