#ifndef AKTUALIZR_LITE_INSTALLER_H_
#define AKTUALIZR_LITE_INSTALLER_H_

#include "aktualizr-lite/api.h"
#include "libaktualizr/types.h"

class Installer {
 public:
  // TODO: use the API's return type - InstallResult
  virtual data::InstallationResult Install(const TufTarget& target, InstallMode mode) = 0;

  virtual ~Installer() = default;
  Installer(const Installer&) = delete;
  Installer(const Installer&&) = delete;
  Installer& operator=(const Installer&) = delete;
  Installer& operator=(const Installer&&) = delete;

 protected:
  explicit Installer() = default;
};

#endif  // AKTUALIZR_LITE_DOWNLOADER_H_
