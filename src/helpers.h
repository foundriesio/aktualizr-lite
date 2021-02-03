#ifndef AKTUALIZR_LITE_HELPERS_
#define AKTUALIZR_LITE_HELPERS_

#include <string>

#include "liteclient.h"

struct Version {
  std::string raw_ver;
  Version(std::string version) : raw_ver(std::move(version)) {}

  bool operator<(const Version& other) { return strverscmp(raw_ver.c_str(), other.raw_ver.c_str()) < 0; }
};

bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags);

#endif  // AKTUALIZR_LITE_HELPERS_
