#ifndef MASKED_ROLLBACK_H_
#define MASKED_ROLLBACK_H_

#include "rollback.h"
#include "utilities/utils.h"

class MaskedRollback : public Rollback {
 public:
  MaskedRollback() : Rollback() {}

  void setBootOK() {
    std::string sink;
    if (Utils::shell("fw_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fw_setenv upgrade_available 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting upgrade_available for u-boot";
    }
  }

  void updateNotify() {
    std::string sink;
    if (Utils::shell("fw_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fw_setenv upgrade_available 1", &sink) != 0) {
      LOG_WARNING << "Failed setting upgrade_available for u-boot";
    }
    if (Utils::shell("fw_setenv rollback 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting rollback flag";
    }
  }
};

#endif
