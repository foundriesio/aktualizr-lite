#ifndef AKTUALIZR_LITE_ROLLBACK_H_
#define AKTUALIZR_LITE_ROLLBACK_H_

#include <fstream>

#include <boost/algorithm/algorithm.hpp>
#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "utilities/utils.h"

class Rollback {
 public:
  constexpr static const char* const VersionFile{"/usr/lib/firmware/version.txt"};

  explicit Rollback(std::string deployment_dir = "/ostree/deploy/lmp/deploy/")
      : deployment_dir_{std::move(deployment_dir)} {}
  virtual ~Rollback() = default;
  Rollback(const Rollback&) = delete;
  Rollback(Rollback&&) = delete;
  Rollback& operator=(const Rollback&) = delete;
  Rollback& operator=(Rollback&&) = delete;

  virtual void setBootOK() {}
  virtual void updateNotify() {}
  virtual void installNotify(const Uptane::Target& target) { (void)target; }

 protected:
  std::string getVersion(const Uptane::Target& target) {
    try {
      std::string file;
      for (auto& p : boost::filesystem::directory_iterator(deployment_dir_)) {
        std::string dir = p.path().string();
        if (!boost::filesystem::is_directory(dir)) {
          continue;
        }
        if (boost::algorithm::contains(dir, target.sha256Hash())) {
          file = dir + VersionFile;
          break;
        }
      }
      if (file.empty()) {
        LOG_WARNING << "Target hash not found";
        return std::string();
      }

      LOG_INFO << "Target firmware file: " << file;
      std::ifstream ifs(file);
      std::string version((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
      std::string watermark = "bootfirmware_version";
      std::string::size_type i = version.find(watermark);
      if (i != std::string::npos) {
        version.erase(i, watermark.length() + 1);
        LOG_INFO << "Target firmware version: " << version;
        return version;
      } else {
        LOG_WARNING << "Target firmware version not found";
        return std::string();
      }
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to obtain Target firmware version:  " << exc.what();
      return "";
    }
  }

  static void increaseBootUpgradeAvailable(const std::string& get_cmd, const std::string& set_cmd) {
    std::string ba_str{"0"};
    if (Utils::shell(get_cmd + " bootupgrade_available", &ba_str) != 0) {
      LOG_WARNING << "Failed to read bootupgrade_available, assume it is set to 0";
    }
    int bootupgrade_available{0};
    try {
      bootupgrade_available = std::stoi(ba_str);
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to convert `bootupgrade_available` value: " << exc.what() << "; assume it is set to 0";
    }
    ++bootupgrade_available;
    std::string sink;
    if (Utils::shell(set_cmd + " bootupgrade_available " + std::to_string(bootupgrade_available), &sink) == 0) {
      LOG_INFO << "bootupgrade_available is set to " << bootupgrade_available;
    } else {
      LOG_WARNING << "Failed to set bootupgrade_available";
    }
  }

 private:
  const std::string deployment_dir_;
};

#endif  // AKTUALIZR_LITE_ROLLBACK_H_
