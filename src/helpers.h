#ifndef AKTUALIZR_LITE_HELPERS_
#define AKTUALIZR_LITE_HELPERS_

#include <string>

#include "liteclient.h"

void log_info_target(const std::string& prefix, const Config& config, const Uptane::Target& t);

#endif  // AKTUALIZR_LITE_HELPERS_
