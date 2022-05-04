#include "ostree_puller.h"

#include "ostree/repo.h"

namespace OSTree { namespace offline {

DownloadResult OstreePuller::Download(const TufTarget& target) {
  Repo repo{sysroot_->path() +  "/ostree/repo"};
  try {
    repo.pullLocal(src_repo_path_, target.Sha256Hash());
  } catch (const std::exception& exc) {
    LOG_ERROR << "Offline ostree download failed: " << exc.what();
    return DownloadResult{DownloadResult::Status::DownloadFailed, exc.what()};
  }

  return DownloadResult{DownloadResult::Status::Ok, ""};
}


} // namespace offline
} // namespace OSTree
