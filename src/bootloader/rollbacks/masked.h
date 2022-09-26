#ifndef AKTUALIZR_LITE_MASKED_ROLLBACK_H_
#define AKTUALIZR_LITE_MASKED_ROLLBACK_H_

#include "rollback.h"
#include "utilities/utils.h"

class MaskedRollback : public Rollback {
 public:
  explicit MaskedRollback(const std::string& deployment_path) : Rollback(deployment_path) {}

  void setBootOK() override {
    std::string sink;
    if (Utils::shell("fw_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fw_setenv upgrade_available 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting upgrade_available for u-boot";
    }
  }

  void updateNotify() override {
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

  void installNotify(const Uptane::Target& target) override {
    std::string version = getVersion(target);
    if (version.empty()) {
      return;
    }
    std::string sink;
    if (Utils::shell("fw_printenv -n bootfirmware_version", &sink) != 0) {
      LOG_WARNING << "Failed to read bootfirmware_version";
      return;
    }
    LOG_INFO << "Current bootfirmware version: " << sink;
    if (sink == version) {
      LOG_INFO << "No new bootfirmware version is found";
      return;
    }
    LOG_INFO << "New bootfirmware is found: " << version;
    increaseBootUpgradeAvailable("fw_printenv -n", "fw_setenv");
  }
};

#endif  // AKTUALIZR_LITE_MASKED_ROLLBACK_H_
