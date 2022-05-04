#ifndef AKTUALIZR_LITE_OFFLINE_APPS_PULLER_H_
#define AKTUALIZR_LITE_OFFLINE_APPS_PULLER_H_

#include "downloader.h"
#include "appengine.h"


namespace Docker { namespace offline {

class AppsPuller: public Downloader {
 public:
  AppsPuller(boost::filesystem::path src_root, boost::filesystem::path skopeo_store_root):
    src_root_{std::move(src_root)},
    store_root_{std::move(skopeo_store_root)} {}

  DownloadResult Download(const TufTarget& target) override;

  private:
    const boost::filesystem::path src_root_;

    const boost::filesystem::path src_apps_root_{src_root_ / "skopeo" / "apps"};
    const boost::filesystem::path src_blobs_{src_root_ / "skopeo" / "blobs"};
    const boost::filesystem::path src_docker_root_{src_root_ / "docker"};

    const boost::filesystem::path store_root_;
    const boost::filesystem::path dst_apps_root_{store_root_ / "apps"};
    const boost::filesystem::path dst_blobs_{store_root_ / "blobs"};
};


} // offline
} // Docker


#endif // AKTUALIZR_LITE_OFFLINE_APPS_PULLER_H_
