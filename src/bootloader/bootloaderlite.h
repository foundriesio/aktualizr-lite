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

  explicit BootloaderLite(BootloaderConfig config, INvStorage& storage, OSTree::Sysroot::Ptr sysroot);

  void installNotify(const Uptane::Target& target) const override;

 private:
  OSTree::Sysroot::Ptr sysroot_;
};

}  // namespace bootloader

#endif  // AKTUALIZR_LITE_BOOTLOADERLITE_H_
