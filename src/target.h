#ifndef AKTUALIZR_LITE_TARGET_H_
#define AKTUALIZR_LITE_TARGET_H_

#include <boost/optional.hpp>

#include "uptane/tuf.h"

// This is the prototype of Target class of the future aklite's UpdateAgent that wraps the libaktualizr pack manager
class Target {
 public:
  static constexpr const char* const TagField{"tags"};

  struct Version {
    std::string raw_ver;
    Version(std::string version) : raw_ver(std::move(version)) {}

    bool operator<(const Version& other) { return strverscmp(raw_ver.c_str(), other.raw_ver.c_str()) < 0; }
  };

 public:
  static bool hasTag(const Uptane::Target& target, const std::vector<std::string>& tags);
  static void setCorrelationID(Uptane::Target& target);
};

#endif  // AKTUALIZR_LITE_TARGET_H_
