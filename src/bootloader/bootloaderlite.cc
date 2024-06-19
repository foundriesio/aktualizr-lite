#include "bootloaderlite.h"

#include <boost/algorithm/string.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>

#include "bootloader/bootloader.h"
#include "ostree/repo.h"
#include "storage/invstorage.h"
#include "utilities/exceptions.h"

namespace bootloader {

static const std::unordered_map<RollbackMode, std::string> setCmds{
    {RollbackMode::kUbootMasked, "fw_setenv"},
    {RollbackMode::kFioVB, "fiovb_setenv"},
    {RollbackMode::kFioEFI, "fioefi_setenv"},
};
static const std::unordered_map<RollbackMode, std::string> getCmds{
    {RollbackMode::kUbootMasked, "fw_printenv -n"},
    {RollbackMode::kFioVB, "fiovb_printenv"},
    {RollbackMode::kFioEFI, "fioefi_printenv"},
};
static const std::string noneCmd;

BootloaderLite::BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot)
    : Bootloader(std::move(config), storage),
      sysroot_{std::move(sysroot)},
      get_env_cmd_{getCmds.count(config_.rollback_mode) == 1 ? getCmds.at(config_.rollback_mode) : noneCmd},
      set_env_cmd_{setCmds.count(config_.rollback_mode) == 1 ? setCmds.at(config_.rollback_mode) : noneCmd} {}

void BootloaderLite::installNotify(const Uptane::Target& target) const {
  std::string sink;
  switch (config_.rollback_mode) {
    case RollbackMode::kBootloaderNone:
    case RollbackMode::kUbootGeneric:
      break;
    case RollbackMode::kFioEFI:
          env_cmd_vars_ = boost::str(boost::format("%s=%s") % OstreeTargetPathVar %
                                     getTargetDir(sysroot_->deployment_path(), target.sha256Hash()));
    case RollbackMode::kUbootMasked:
    case RollbackMode::kFioVB: {
      const auto target_version_val{getDeploymentVersion(target.sha256Hash())};
      if (!std::get<1>(target_version_val)) {
        LOG_WARNING << "Failed to get the Target's bootloader version, skipping bootloader update";
        return;
      }
      const std::string& target_version{std::get<0>(target_version_val)};

      std::string cur_version;
      bool is_current_ver_valid;
      std::tie(cur_version, is_current_ver_valid) = getCurrentVersion();
      if (!is_current_ver_valid) {
        LOG_WARNING << "Failed to get current bootloader version: " << cur_version;
      }
      const auto is_rollback_protected{isRollbackProtectionEnabled()};
      // The rollback check is done before the installation in verifyBootloaderUpdate(),
      // so, at this stage, we just check whether the versions differ
      if (!is_current_ver_valid || target_version != cur_version) {
        // Set `bootupgrade_available` if:
        // 1. The current bootloader version is unknown and the given target's bootloader version is
        //    known and valid (checked at the method beginning).
        // or
        // 2. The current bootloader version is known and valid and
        //    the given target's bootloader version doesn't equal to the current one.
        //    If the rollback protection is ON then the given target's bootloader version
        //    must be higher than the current one, what is verified before this method is called;
        //    see RootfsTreeManager::install -> RootfsTreeManager::verifyBootloaderUpdate.
        //    Therefore, it's suffice to check whether the versions match or not at this point.
        const auto cur_ver_str{is_current_ver_valid ? cur_version : "unknown"};
        const auto set_bu_res{setEnvVar("bootupgrade_available", "1")};
        if (std::get<1>(set_bu_res)) {
          LOG_INFO << "Bootloader will be updated from version " << cur_ver_str << " to " << target_version
                   << "; rollback protection: " << (is_rollback_protected ? "ON" : "OFF");
        } else {
          LOG_ERROR << "Failed to set `bootupgrade_available`, skipping bootloader update; "
                    << "current version: " << cur_ver_str << ", Target's version: " << target_version
                    << ", rollback protection: " << (is_rollback_protected ? "ON" : "OFF")
                    << ", err: " << std::get<0>(set_bu_res);
        }
      } else {
        LOG_INFO << "Skipping bootloader update; current version: " << cur_version
                 << ", Target's version: " << target_version
                 << ", rollback protection: " << (is_rollback_protected ? "ON" : "OFF");
      }
    } break;
    default:
      throw NotImplementedException();
  }
}

BootloaderLite::VersionStrRes BootloaderLite::getDeploymentVersion(const std::string& hash) const {
  const auto ver_str{getVersion(sysroot_->deployment_path(), hash)};
  return {ver_str, !ver_str.empty()};
}

std::string BootloaderLite::getVersion(const std::string& deployment_dir, const std::string& hash,
                                       const std::string& ver_file) {
  try {
    std::string target_dir = getTargetDir(deployment_dir, hash);

    if (target_dir.empty()) {
      LOG_WARNING << "Target's system root directory is not found for hash: " << hash <<
                   " in: " << deployment_dir;
      return std::string();
    }

    std::string file = target_dir + ver_file;
    if (!boost::filesystem::exists(file)) {
      LOG_WARNING << "Target's bootloader version file is not found, expected: " << file;
      return std::string();
    }

    LOG_DEBUG << "Target firmware file: " << file;
    std::ifstream ifs(file);
    std::string version_line((std::istreambuf_iterator<char>(ifs)), std::istreambuf_iterator<char>());
    return extractVersionValue(version_line);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to get Target firmware version:  " << exc.what();
    return "";
  }
}

std::string BootloaderLite::getTargetDir(const std::string& deployment_dir,
                                         const std::string& target_hash) {
  for (auto& p : boost::filesystem::directory_iterator(deployment_dir)) {
    std::string dir = p.path().string();
    if (!boost::filesystem::is_directory(dir)) {
      continue;
    }
    if (boost::algorithm::contains(dir, target_hash)) {
      return dir;
    }
  }

  return "";
}

bool BootloaderLite::isUpdateInProgress() const {
  if (!isUpdateSupported()) {
    // update is not supported by a given bootloader type
    LOG_DEBUG << "Update is not supported by a given bootloader type (" << config_.rollback_mode << "),"
              << " assuming that no bootloader update is in progress.";
    return false;
  }
  const auto ba_val{getEnvVar("bootupgrade_available")};
  if (!std::get<1>(ba_val)) {
    LOG_ERROR << "Failed to get `bootupgrade_available` value, assuming it is set to 0; err: " << std::get<0>(ba_val);
    return false;
  }
  return std::get<0>(ba_val) == "1";
}

bool BootloaderLite::isRollbackProtectionEnabled() const {
  if (!isUpdateSupported()) {
    // update is not supported by a given bootloader type, hence no rolback protection support
    LOG_DEBUG << "Update is not supported by a given bootloader type (" << config_.rollback_mode << "),"
              << " assuming that rollback protection is disabled";
    return false;
  }
  const auto rb_val{getEnvVar("rollback_protection")};
  if (!std::get<1>(rb_val)) {
    LOG_ERROR << "Failed to get `rollback_protection` value, assuming it is turned off; err: " << std::get<0>(rb_val);
    return false;
  }
  return std::get<0>(rb_val) == "1";
}

std::string BootloaderLite::getTargetVersion(const std::string& target_hash) const {
  const OSTree::Repo repo{sysroot_->repoPath()};
  const std::string version_line{repo.readFile(target_hash, VersionFile)};
  return extractVersionValue(version_line);
}

std::tuple<std::string, bool> BootloaderLite::setEnvVar(const std::string& var_name, const std::string& var_val) const {
  if (set_env_cmd_.empty()) {
    const auto er_msg{
        boost::format("No command to set an environment variable found for the given bootloader type: `%s`") %
        config_.rollback_mode};
    return {er_msg.str(), false};
  }
  const auto cmd{boost::format{"%s %s %s %s"} % env_cmd_vars_ % set_env_cmd_ % var_name % var_val};
  std::string output;
  if (Utils::shell(cmd.str(), &output) != 0) {
    const auto er_msg{boost::format("Failed to set a bootloader environment variable; cmd: %s, err: %s") % cmd %
                      output};
    return {er_msg.str(), false};
  }
  return {"", true};
}

std::tuple<std::string, bool> BootloaderLite::getEnvVar(const std::string& var_name) const {
  if (get_env_cmd_.empty()) {
    const auto er_msg{
        boost::format("No command to read an environment variable found for the given bootloader type: `%s`") %
        config_.rollback_mode};
    return {er_msg.str(), false};
  }
  const auto cmd{boost::format{"%s %s %s"} % env_cmd_vars_ % get_env_cmd_ % var_name};
  std::string output;
  if (Utils::shell(cmd.str(), &output) != 0) {
    const auto er_msg{boost::format("Failed to get a bootloader environment variable; cmd: %s, err: %s") % cmd %
                      output};
    return {er_msg.str(), false};
  }
  boost::trim_if(output, boost::is_any_of(" \t\r\n"));
  return {output, true};
}

BootloaderLite::VersionNumbRes BootloaderLite::verStrToNumber(const std::string& ver_str) {
  VersionNumbRes res{0, false};
  try {
    res = {boost::lexical_cast<VersionType>(ver_str), true};
  } catch (const boost::bad_lexical_cast& cast_err) {
    LOG_ERROR << "Invalid format of the boot firmware version; value: " << ver_str << "; err: " << cast_err.what();
    res = {0, false};
  }
  return res;
}

std::string BootloaderLite::extractVersionValue(const std::string& version_line) {
  std::string version{version_line};
  const std::string watermark{"bootfirmware_version"};
  const std::string::size_type ver_pos{version.find(watermark)};
  if (ver_pos == std::string::npos) {
    throw std::invalid_argument("Failed to parse the bootloader version line; no `" + watermark + "` is found in `" +
                                version_line + "`");
  }
  version.erase(ver_pos, watermark.length() + 1);
  boost::trim_if(version, boost::is_any_of(" \t\r\n"));
  return version;
}

}  // namespace bootloader
