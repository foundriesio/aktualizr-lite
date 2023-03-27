#ifndef AKTUALIZR_LITE_BOOTLOADERLITE_H_
#define AKTUALIZR_LITE_BOOTLOADERLITE_H_

#include "bootloader/bootloader.h"
#include "libaktualizr/config.h"
#include "ostree/sysroot.h"

class INvStorage;

namespace bootloader {

class BootFwUpdateStatus {
 public:
  BootFwUpdateStatus(const BootFwUpdateStatus&) = delete;
  BootFwUpdateStatus& operator=(const BootFwUpdateStatus&) = delete;
  BootFwUpdateStatus& operator=(BootFwUpdateStatus&&) = delete;
  virtual ~BootFwUpdateStatus() = default;

  virtual bool isUpdateInProgress() const = 0;

 protected:
  BootFwUpdateStatus() = default;
  BootFwUpdateStatus(BootFwUpdateStatus&&) = default;
};

class BootloaderLite : public Bootloader, public BootFwUpdateStatus {
 public:
  static constexpr const char* const VersionFile{"/usr/lib/firmware/version.txt"};

  explicit BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot);

  static std::string getVersion(const std::string& deployment_dir, const std::string& hash,
                                const std::string& ver_file = VersionFile);

  void installNotify(const Uptane::Target& target) const override;

  bool isUpdateInProgress() const override;

 private:
  static int readBootUpgradeAvailable(const std::string& get_cmd);

  OSTree::Sysroot::Ptr sysroot_;
};

}  // namespace bootloader

#endif  // AKTUALIZR_LITE_BOOTLOADERLITE_H_
