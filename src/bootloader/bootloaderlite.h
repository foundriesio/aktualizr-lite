#ifndef AKTUALIZR_LITE_BOOTLOADERLITE_H_
#define AKTUALIZR_LITE_BOOTLOADERLITE_H_

#include "bootloader/bootloader.h"
#include "libaktualizr/config.h"
#include "ostree/sysroot.h"

class INvStorage;

namespace bootloader {

class BootloaderLite : public Bootloader {
 public:
  static constexpr const char* const VersionFile{"/usr/lib/firmware/version.txt"};
  static constexpr const char* const VersionTitle{"bootfirmware_version"};

  explicit BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot,
                          std::string ver_file_path = VersionFile, std::string ver_title = VersionTitle);

  void installNotify(const Uptane::Target& target) const override;

  static std::string getVersion(const std::string& deployment_dir, const std::string& ver_file_path,
                                const std::string& ver_title, const std::string& hash);
  static std::string readVersion(const boost::filesystem::path& ver_file, const std::string& ver_title);
  static int readBootUpgradeAvailable(const std::string& get_cmd);
  static void setBootUpgradeAvailable(const std::string& set_cmd, int val);

 private:
  using GetSetCmd = std::tuple<std::string, std::string>;

  void setBootUpgradeFlag(const std::string& hash, const GetSetCmd&& cmd) const;

  OSTree::Sysroot::Ptr sysroot_;
  const std::string ver_file_path_;
  const std::string ver_title_;
};

}  // namespace bootloader

#endif  // AKTUALIZR_LITE_BOOTLOADERLITE_H_
