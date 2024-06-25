#ifndef AKTUALIZR_LITE_DAEMON_H_
#define AKTUALIZR_LITE_DAEMON_H_

#include <cstdint>

#include "aktualizr-lite/api.h"

int run_daemon(LiteClient& client, uint64_t interval, bool return_on_sleep);

#endif  // AKTUALIZR_LITE_DAEMON_H_
