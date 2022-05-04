#include "apps_puller.h"

#include <boost/format.hpp>

#include "docker/docker.h"
#include "docker/composeinfo.h"
#include "target.h"
#include "exec.h"


namespace Docker { namespace offline {


DownloadResult AppsPuller::Download(const TufTarget& target) {
  const std::string client{"skopeo"};
  const std::string format{"v2s2"};

  boost::filesystem::create_directories(store_root_);

  for (const auto& app: Target::Apps(target)) {
    if (app.name != "app-05") {
      continue;
    }

    const Uri uri{Uri::parseUri(app.uri, false)};

    const auto src_app_dir{src_apps_root_ / uri.app / uri.digest.hash() };
    const auto dst_app_dir{dst_apps_root_ / uri.app / uri.digest.hash() };

    const auto src_app_manifest{src_app_dir / "manifest.json" };
    const auto dst_app_manifest{dst_app_dir / "manifest.json" };

    LOG_ERROR << "Fetching: " << src_app_manifest;
    if (!boost::filesystem::exists(src_app_manifest)) {
      return DownloadResult{DownloadResult::Status::DownloadFailed, "App manifest not found"};
    }

    const std::string manifest_str{Utils::readFile(src_app_manifest)};
    const Manifest manifest{manifest_str};
    Docker::Uri archive_uri{uri.createUri(HashedDigest(manifest.archiveDigest()))};
    const auto dst_archive_full_path{dst_app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};
    const auto src_archive_full_path{src_app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

    boost::filesystem::create_directories(dst_app_dir);
    exec("/usr/bin/cp", "failed to copy manifest", src_app_manifest, dst_app_manifest);
    exec("/usr/bin/cp", "failed to copy archive", src_archive_full_path, dst_archive_full_path);


    // extract docker-compose.yml, temporal hack, we don't need to extract it
    exec(boost::format{"tar -xzf %s %s"} % dst_archive_full_path % "docker-compose.yml", "no compose file found in archive",
       boost::process::start_dir = dst_app_dir);


    // pull App images
    const auto dst_app_images_dir{dst_app_dir / "images"};
    boost::filesystem::create_directories(dst_app_images_dir);

    const auto compose{ComposeInfo((dst_app_dir / "docker-compose.yml").string())};
    for (const auto& service : compose.getServices()) {
      const auto image_uri = compose.getImage(service);

      const Uri uri{Uri::parseUri(image_uri, false)};
      const auto image_dir{dst_app_images_dir / uri.registryHostname / uri.repo / uri.digest.hash()};
      const auto src_image_dir{src_app_dir/ "images" / uri.registryHostname / uri.repo / uri.digest.hash()};

      LOG_INFO << uri.app << ": downloading image from Registry if missing: " << image_uri << " --> " << image_dir;
      boost::filesystem::create_directories(image_dir);
      std::string client{"/usr/bin/skopeo"};
      std::string format{"v2s2"};

      exec(boost::format{"%s copy -f %s --dest-shared-blob-dir %s --src-shared-blob-dir %s oci:%s oci:%s"} % client % format %
           dst_blobs_ % src_blobs_ % src_image_dir.string() % image_dir.string(), "failed to pull image");

//      const auto image_tar{src_docker_root_ / (uri.digest.hash() + ".tar")};
//      exec(boost::format{"docker load -i %s"} % image_tar.string(), "failed to load images into docker store");
    }
  }

  return DownloadResult{DownloadResult::Status::Ok, ""};
}


} // offline
} // Docker
