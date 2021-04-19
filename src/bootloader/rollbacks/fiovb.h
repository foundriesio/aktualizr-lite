#ifndef FIOVB_ROLLBACK_H_
#define FIOVB_ROLLBACK_H_

#include "rollback.h"
#include "utilities/utils.h"

class FiovbRollback : public Rollback {
 public:
  FiovbRollback() : Rollback() {}

  void setBootOK() {
    std::string sink;
    if (Utils::shell("fiovb_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fiovb_setenv upgrade_available 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting upgrade_available";
    }
  }

  void updateNotify() {
    std::string sink;
    if (Utils::shell("fiovb_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fiovb_setenv upgrade_available 1", &sink) != 0) {
      LOG_WARNING << "Failed setting upgrade_available";
    }
    if (Utils::shell("fiovb_setenv rollback 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting rollback flag";
    }
    if (Utils::shell("fiovb_setenv bootupgrade_available 1", &sink) != 0) {
      LOG_WARNING << "Failed to set bootupgrade_available";
    }
  }
};

#endif
