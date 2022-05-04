#ifndef AKTUALIZR_LITE_OFFLINE_OSTREE_PULLER_H_
#define AKTUALIZR_LITE_OFFLINE_OSTREE_PULLER_H_

#include "downloader.h"
#include "ostree/sysroot.h"

namespace OSTree { namespace offline {


class OstreePuller: public Downloader {
 public:
  OstreePuller(std::shared_ptr<Sysroot> sysroot, std::string src_repo_path):
    Downloader(), sysroot_{std::move(sysroot)}, src_repo_path_{std::move(src_repo_path)} {}

  DownloadResult Download(const TufTarget& target) override;
 private:

  std::shared_ptr<Sysroot> sysroot_;
  const std::string src_repo_path_;
};


} // namespace offline
} // namespace OSTree

#endif // AKTUALIZR_LITE_OFFLINE_OSTREE_PULLER_H_
