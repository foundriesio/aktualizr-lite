#ifndef AKTUALIZR_LITE_FIOVB_ROLLBACK_H_
#define AKTUALIZR_LITE_FIOVB_ROLLBACK_H_

#include "rollback.h"
#include "utilities/utils.h"

class FiovbRollback : public Rollback {
 public:
  explicit FiovbRollback(const std::string& deployment_path) : Rollback(deployment_path) {}

  void setBootOK() override {
    std::string sink;
    if (Utils::shell("fiovb_setenv bootcount 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting bootcount";
    }
    if (Utils::shell("fiovb_setenv upgrade_available 0", &sink) != 0) {
      LOG_WARNING << "Failed resetting upgrade_available";
    }
  }

  void updateNotify() override {
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
  }

  void installNotify(const Uptane::Target& target) override {
    std::string version = getVersion(target);
    if (version.empty()) {
      return;
    }
    std::string sink;
    if (Utils::shell("fiovb_printenv bootfirmware_version", &sink) != 0) {
      LOG_WARNING << "Failed to read bootfirmware_version";
      sink = std::string();
    }
    LOG_INFO << "Current firmware version: " << sink;
    if (sink != version) {
      LOG_INFO << "Update firmware to version: " << version;
      if (Utils::shell("fiovb_setenv bootupgrade_available 1", &sink) != 0) {
        LOG_WARNING << "Failed to set bootupgrade_available";
      }
    }
  }
};

#endif  // AKTUALIZR_LITE_FIOVB_ROLLBACK_H_
