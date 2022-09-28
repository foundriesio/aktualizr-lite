#include "bootloaderlite.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>

#include "bootloader/bootloader.h"
#include "storage/invstorage.h"
#include "utilities/exceptions.h"

namespace bootloader {

BootloaderLite::BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot,
                               std::string ver_file_path, std::string ver_title)
    : Bootloader(std::move(config), storage),
      sysroot_{std::move(sysroot)},
      ver_file_path_{std::move(ver_file_path)},
      ver_title_{std::move(ver_title)} {}

void BootloaderLite::installNotify(const Uptane::Target& target) const {
  std::string sink;
  switch (config_.rollback_mode) {
    case RollbackMode::kBootloaderNone:
    case RollbackMode::kUbootGeneric:
      break;
    case RollbackMode::kUbootMasked:
      setBootUpgradeFlag(target.sha256Hash(), {"fw_printenv -n", "fw_setenv"});
      break;
    case RollbackMode::kFioVB:
      setBootUpgradeFlag(target.sha256Hash(), {"fiovb_printenv", "fiovb_setenv"});
      break;
    default:
      throw NotImplementedException();
  }
}

void BootloaderLite::setBootUpgradeFlag(const std::string& hash, const GetSetCmd&& cmd) const {
  const auto new_ver{getVersion(sysroot_->deployment_path(), ver_file_path_, ver_title_, hash)};
  if (!new_ver.empty()) {
    LOG_INFO << "New Target's bootfirmware version: " << new_ver;
  }
  const auto cur_ver{readVersion(sysroot_->path() + ver_file_path_, ver_title_)};
  if (!cur_ver.empty()) {
    LOG_INFO << "Current bootfirmware version: " << cur_ver;
  }

  int bootupgrade_available{readBootUpgradeAvailable(std::get<0>(cmd))};

  if (!new_ver.empty() && new_ver != cur_ver) {
    // If a new Target includes a boot firmware (`version.txt` is present and  contains correct version value),
    // and the boot fw version is different from the current fw version, then increase `bootupgrade_available`.

    LOG_INFO << "Increasing a value of the bootloader flag `bootupgrade_available` to indicate that "
             << "the new bootfirmware version is available;"
             << " current: " << (cur_ver.empty() ? "unknown" : cur_ver)
             << " new: " << (new_ver.empty() ? "unknown" : new_ver);

    LOG_INFO << "Current `bootupgrade_available`: " << bootupgrade_available;
    ++bootupgrade_available;
    LOG_INFO << "Setting `bootupgrade_available` to: " << bootupgrade_available;
    setBootUpgradeAvailable(std::get<1>(cmd), bootupgrade_available);
    return;
  }

  if (bootupgrade_available == 0) {
    return;
  }

  std::string sink;
  LOG_INFO
      << "Decreasing a value of the bootloader flag `bootupgrade_available` since no new bootfirmware version found;"
      << " current: " << (cur_ver.empty() ? "unknown" : cur_ver)
      << " new Target's: " << (new_ver.empty() ? "unknown" : new_ver);
  LOG_INFO << "Current `bootupgrade_available`: " << bootupgrade_available;
  --bootupgrade_available;
  LOG_INFO << "Setting `bootupgrade_available` to: " << bootupgrade_available;
  setBootUpgradeAvailable(std::get<1>(cmd), bootupgrade_available);
}

boost::filesystem::path findVersionFileInDeployment(const std::string& deployment_dir, const std::string& ver_file_path,
                                                    const std::string& deployment_hash) {
  boost::filesystem::path found_depl_ver_file;
  for (auto& p : boost::filesystem::directory_iterator(deployment_dir)) {
    if (!boost::filesystem::is_directory(p)) {
      continue;
    }
    if (boost::algorithm::starts_with(p.path().filename().string(), deployment_hash)) {
      found_depl_ver_file = p / ver_file_path;
      break;
    }
  }
  return found_depl_ver_file;
}

std::string BootloaderLite::getVersion(const std::string& deployment_dir, const std::string& ver_file_path,
                                       const std::string& ver_title, const std::string& hash) {
  std::string res_ver;
  try {
    const auto ver_file{findVersionFileInDeployment(deployment_dir, ver_file_path, hash)};
    if (ver_file.empty()) {
      LOG_INFO << "Bootfirmware version file has not been found in the Target's deployment; "
               << "deployment dir: " << deployment_dir << "; hash: " << hash;
      return "";
    }
    res_ver = readVersion(ver_file, ver_title);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to get a bootfirmware version: " << exc.what();
  }
  return res_ver;
}

std::string BootloaderLite::readVersion(const boost::filesystem::path& ver_file, const std::string& ver_title) {
  std::string res_ver;
  try {
    const auto ver_str{Utils::readFile(ver_file)};
    std::string::size_type i = ver_str.find(ver_title);
    if (i != std::string::npos) {
      res_ver = ver_str.substr(i + ver_title.size() + 1);
    }
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to read a bootfirmware version from the file: " << ver_file << "; err: " << exc.what();
  }
  return res_ver;
}

int BootloaderLite::readBootUpgradeAvailable(const std::string& get_cmd) {
  int bootupgrade_available{0};
  std::string ba_str{"0"};

  try {
    if (Utils::shell(get_cmd + " bootupgrade_available", &ba_str) != 0) {
      LOG_ERROR << "Failed to read bootupgrade_available, assume it is set to 0";
    }
    bootupgrade_available = std::stoi(ba_str);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to get `bootupgrade_available` value: " << exc.what() << "; assume it is set to 0";
  }
  return bootupgrade_available;
}

void BootloaderLite::setBootUpgradeAvailable(const std::string& set_cmd, int val) {
  std::string sink;
  if (Utils::shell(set_cmd + " bootupgrade_available " + std::to_string(val), &sink) != 0) {
    LOG_ERROR << "Failed to set bootupgrade_available";
  }
}

}  // namespace bootloader
