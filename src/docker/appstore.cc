#include "appstore.h"
#include "docker.h"

#include <boost/format.hpp>
#include <boost/process.hpp>


namespace Docker {

SkopeoAppStore::SkopeoAppStore(std::string skopeo_bin, boost::filesystem::path root):root_{std::move(root)}, skopeo_bin_{std::move(skopeo_bin)} {

  boost::filesystem::create_directories(apps_root_);
  boost::filesystem::create_directories(images_root_);
  boost::filesystem::create_directories(images_blobs_root_);

}

boost::filesystem::path SkopeoAppStore::appArchive(const AppEngine::App& app) const {
  Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  return appRoot(app) / (uri.digest.shortHash() + '.' + app.name + ArchiveExt);
}

bool SkopeoAppStore::pullFromRegistry(const std::string& uri, const std::string& auth) const {
  std::string cmd;
  const std::string format{ManifestFormat};

  const Uri uri_parts{Uri::parseUri(uri)};
  const auto dst_path{images_root_ / uri_parts.registryHostname / uri_parts.repo / uri_parts.digest.hash()};

  boost::filesystem::create_directories(dst_path);

  if (auth.empty()) {
    // use REGISTRY_AUTH_FILE env var for the aktualizr service to setup
    // access to private Docker Registries, e.g.
    // export REGISTRY_AUTH_FILE=/usr/lib/docker/config.json
    // The file lists docker cred helpers
    boost::format cmd_fmt{"%s copy -f %s --dest-shared-blob-dir %s docker://%s oci:%s"};
    cmd = boost::str(cmd_fmt % skopeo_bin_ % format % images_blobs_root_ % uri % dst_path.string());
  } else {
    boost::format cmd_fmt{"%s copy -f %s --src-creds %s --dest-shared-blob-dir %s docker://%s oci:%s"};
    cmd = boost::str(cmd_fmt % skopeo_bin_ % format % auth % images_blobs_root_ % uri % dst_path.string());
  }


  return runCmd(cmd);
}


bool SkopeoAppStore::copyToDockerStore(const std::string& uri) const {
  const Uri uri_parts{Uri::parseUri(uri)};
  const auto src_path{images_root_ / uri_parts.registryHostname / uri_parts.repo / uri_parts.digest.hash()};
  const std::string format{ManifestFormat};
  const std::string tag{uri_parts.registryHostname + '/' + uri_parts.repo + ':' + uri_parts.digest.shortHash()};

  boost::format cmd_fmt{"%s copy -f %s --src-shared-blob-dir %s oci:%s docker-daemon:%s"};
  std::string cmd{boost::str(cmd_fmt % skopeo_bin_ % format % images_blobs_root_ % src_path.string() % tag)};
  return runCmd(cmd);
}

bool SkopeoAppStore::runCmd(const std::string& cmd) {
  int exit_code = boost::process::system(cmd, boost::this_process::environment());
  return exit_code == 0;
}

} // Docker
