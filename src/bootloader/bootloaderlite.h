#ifndef AKTUALIZR_LITE_BOOTLOADERLITE_H_
#define AKTUALIZR_LITE_BOOTLOADERLITE_H_

#include "bootloader/bootloader.h"
#include "libaktualizr/config.h"
#include "rollbacks/rollback.h"

class INvStorage;

class BootloaderLite : public Bootloader {
 public:
  BootloaderLite(BootloaderConfig config, INvStorage& storage);
  virtual ~BootloaderLite() {}
  void setBootOK() const override;
  void updateNotify() const override;
  void installNotify(const Uptane::Target& target) const override;

 private:
  std::unique_ptr<Rollback> rollback_;
};

#endif  // AKTUALIZR_LITE_BOOTLOADERLITE_H_
