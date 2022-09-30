#include "bootloaderlite.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "bootloader/bootloader.h"
#include "storage/invstorage.h"
#include "utilities/exceptions.h"

namespace bootloader {

std::string getVersion(const std::string& deployment_dir, const std::string& ver_file, const std::string& hash);

BootloaderLite::BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot)
    : Bootloader(std::move(config), storage), sysroot_{std::move(sysroot)} {}

void BootloaderLite::installNotify(const Uptane::Target& target) const {
  std::string sink;
  switch (config_.rollback_mode) {
    case RollbackMode::kBootloaderNone:
    case RollbackMode::kUbootGeneric:
      break;
    case RollbackMode::kUbootMasked: {
      std::string version = getVersion(sysroot_->deployment_path(), VersionFile, target.sha256Hash());
      if (version.empty()) {
        return;
      }
      std::string sink;
      if (Utils::shell("fw_printenv -n bootfirmware_version", &sink) != 0) {
        LOG_WARNING << "Failed to read bootfirmware_version";
        return;
      }
      LOG_INFO << "Current boot firmware version: " << sink;
      if (sink != version) {
        LOG_INFO << "Update firmware to version: " << version;
        if (Utils::shell("fw_setenv bootupgrade_available 1", &sink) != 0) {
          LOG_WARNING << "Failed to set bootupgrade_available";
        }
      }
    } break;
    case RollbackMode::kFioVB: {
      std::string version = getVersion(sysroot_->deployment_path(), VersionFile, target.sha256Hash());
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
    } break;
    default:
      throw NotImplementedException();
  }
}

std::string getVersion(const std::string& deployment_dir, const std::string& ver_file, const std::string& hash) {
  try {
    std::string file;
    for (auto& p : boost::filesystem::directory_iterator(deployment_dir)) {
      std::string dir = p.path().string();
      if (!boost::filesystem::is_directory(dir)) {
        continue;
      }
      if (boost::algorithm::contains(dir, hash)) {
        file = dir + ver_file;
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

}  // namespace bootloader
