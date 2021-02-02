#ifndef AKTUALIZR_LITE_HELPERS_
#define AKTUALIZR_LITE_HELPERS_

#include <string>

#include "liteclient.h"

struct Version {
  std::string raw_ver;
  Version(std::string version) : raw_ver(std::move(version)) {}

  bool operator<(const Version& other) { return strverscmp(raw_ver.c_str(), other.raw_ver.c_str()) < 0; }
};

void generate_correlation_id(Uptane::Target& t);
bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags);
bool known_local_target(LiteClient& client, const Uptane::Target& t, std::vector<Uptane::Target>& installed_versions);
void log_info_target(const std::string& prefix, const Config& config, const Uptane::Target& t);

#endif  // AKTUALIZR_LITE_HELPERS_
