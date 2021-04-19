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

  void installNotify(const Uptane::Target& target) {
    std::string version = getVersion(target);
    if (version.empty()) {
      return;
    }
    std::string sink;
    if (Utils::shell("fw_printenv bootfirmware_version", &sink) != 0) {
      LOG_WARNING << "Failed to read bootfirmware_version";
      return;
    }
    LOG_INFO << "Current boot firmware version: " << sink;
    if (sink.compare(version) != 0) {
      LOG_INFO << "Update firmware to version: " << version;
      if (Utils::shell("fw_setenv bootupgrade_available 1", &sink) != 0) {
        LOG_WARNING << "Failed to set bootupgrade_available";
      }
    }
  }
};

#endif
