#ifndef AKTUALIZR_LITE_HELPERS_
#define AKTUALIZR_LITE_HELPERS_

#include <string>

#include "liteclient.h"

bool known_local_target(LiteClient& client, const Uptane::Target& t, std::vector<Uptane::Target>& installed_versions);
void get_known_but_not_installed_versions(LiteClient& client,
                                          std::vector<Uptane::Target>& known_but_not_installed_versions);
void log_info_target(const std::string& prefix, const Config& config, const Uptane::Target& t);

#endif  // AKTUALIZR_LITE_HELPERS_
