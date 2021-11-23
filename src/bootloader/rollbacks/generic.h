#ifndef AKTUALIZR_LITE_GENERIC_ROLLBACK_H_
#define AKTUALIZR_LITE_GENERIC_ROLLBACK_H_

#include "rollback.h"
#include "utilities/utils.h"

class GenericRollback : public Rollback {
 public:
  void setBootOK() override {
    std::string sink;
    if (Utils::shell("fw_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
  }

  void updateNotify() override {
    std::string sink;
    if (Utils::shell("fw_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fw_setenv rollback 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting rollback flag";
    }
  }
};

#endif  // AKTUALIZR_LITE_GENERIC_ROLLBACK_H_
