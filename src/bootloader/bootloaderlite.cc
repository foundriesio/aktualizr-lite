#include "bootloaderlite.h"
#include "bootloader/bootloader.h"
#include "rollbacks/factory.h"
#include "storage/invstorage.h"

BootloaderLite::BootloaderLite(BootloaderConfig config, INvStorage& storage)
    : Bootloader(std::move(config), storage), rollback_(RollbackFactory::makeRollback(config_.rollback_mode)) {}

void BootloaderLite::setBootOK() const { rollback_->setBootOK(); }

void BootloaderLite::updateNotify() const { rollback_->updateNotify(); }

void BootloaderLite::installNotify(const Uptane::Target& target) const { rollback_->installNotify(target); }
