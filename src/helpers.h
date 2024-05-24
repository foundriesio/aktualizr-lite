#ifndef AKTUALIZR_LITE_HELPERS_
#define AKTUALIZR_LITE_HELPERS_

#include <string>

#include "liteclient.h"

void generate_correlation_id(Uptane::Target& t);
std::string generate_correlation_id(const TufTarget& t);
bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags);
bool known_local_target(LiteClient& client, const Uptane::Target& t, std::vector<Uptane::Target>& installed_versions);
void get_known_but_not_installed_versions(LiteClient& client,
                                          std::vector<Uptane::Target>& known_but_not_installed_versions);

#endif  // AKTUALIZR_LITE_HELPERS_
