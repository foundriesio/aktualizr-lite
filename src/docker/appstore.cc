#include "appstore.h"
#include "docker.h"

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "composeinfo.h"

namespace Docker {

bool AppStore::runCmd(const std::string& cmd, const boost::filesystem::path& dir) {
  int exit_code = boost::process::system(cmd, boost::this_process::environment(), boost::process::start_dir = dir);
  return exit_code == 0;
}

AppStore::AppStore(boost::filesystem::path root, Docker::RegistryClient::Ptr registry_client)
    : root_{std::move(root)}, registry_client_{std::move(registry_client)} {
  boost::filesystem::create_directories(appsRoot());
  boost::filesystem::create_directories(blobsRoot());
}

boost::filesystem::path AppStore::appRoot(const AppEngine::App& app) const {
  Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  return appsRoot() / app.name / uri.digest.hash();
}

void AppStore::copyApp(const AppEngine::App& app, const boost::filesystem::path& dst) const {
  const auto app_dir{appRoot(app)};
  const auto manifest{Manifest::load(app_dir / Manifest::Filename)};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  const auto cmd = boost::str(boost::format("tar -xzf %s") % archive_full_path);
  if (!runCmd(cmd, dst)) {
    throw std::runtime_error("Failed to copy the compose app: from " + archive_full_path.string() + " to " +
                             dst.string());
  }
}

void AppStore::pullApp(const AppEngine::App& app) {
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{appRoot(app)};
  boost::filesystem::create_directories(app_dir);

  LOG_DEBUG << app.name << ": downloading App from Registry: " << app.uri;

  Manifest manifest{registry_client_->getAppManifest(uri, Manifest::Format)};
  Docker::Uri archive_uri{uri.createUri(manifest.archiveDigest())};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  registry_client_->downloadBlob(archive_uri, archive_full_path, manifest.archiveSize());
  // TODO: verify archive

  manifest.dump(app_dir / Manifest::Filename);
  // extract docker-compose.yml, temporal hack, we don't need to extract it
  auto cmd = boost::str(boost::format("tar -xzf %s %s") % archive_full_path % "docker-compose.yml");
  if (runCmd(cmd, app_dir)) {
    return;
  }

  cmd = boost::str(boost::format("tar -xzf %s %s") % archive_full_path % "./docker-compose.yml");
  if (!runCmd(cmd, app_dir)) {
    throw std::runtime_error("Failed to extract the compose app archive: " + archive_full_path.string());
  }
}

SkopeoAppStore::SkopeoAppStore(std::string skopeo_bin, boost::filesystem::path root,
                               Docker::RegistryClient::Ptr registry_client)
    : AppStore(std::move(root), std::move(registry_client)), skopeo_bin_{std::move(skopeo_bin)} {}

boost::filesystem::path SkopeoAppStore::getAppImageRoot(const AppEngine::App& app, const std::string& uri) const {
  const Uri uri_parts{Uri::parseUri(uri)};
  return appRoot(app) / "images" / uri_parts.registryHostname / uri_parts.repo / uri_parts.digest.hash();
}

bool SkopeoAppStore::pullAppImage(const AppEngine::App& app, const std::string& uri, const std::string& auth) const {
  std::string cmd;
  const std::string format{ManifestFormat};

  const auto dst_path{getAppImageRoot(app, uri)};
  boost::filesystem::create_directories(dst_path);

  if (auth.empty()) {
    // use REGISTRY_AUTH_FILE env var for the aktualizr service to setup
    // access to private Docker Registries, e.g.
    // export REGISTRY_AUTH_FILE=/usr/lib/docker/config.json
    // The file lists docker cred helpers
    boost::format cmd_fmt{"%s copy -f %s --dest-shared-blob-dir %s docker://%s oci:%s"};
    cmd = boost::str(cmd_fmt % skopeo_bin_ % format % blobsRoot() % uri % dst_path.string());
  } else {
    boost::format cmd_fmt{"%s copy -f %s --src-creds %s --dest-shared-blob-dir %s docker://%s oci:%s"};
    cmd = boost::str(cmd_fmt % skopeo_bin_ % format % auth % blobsRoot() % uri % dst_path.string());
  }

  return runCmd(cmd);
}

bool SkopeoAppStore::copyAppImageToDockerStore(const AppEngine::App& app, const std::string& uri) const {
  const Uri uri_parts{Uri::parseUri(uri)};
  const std::string format{ManifestFormat};

  const auto src_path{getAppImageRoot(app, uri)};
  const std::string tag{uri_parts.registryHostname + '/' + uri_parts.repo + ':' + uri_parts.digest.shortHash()};

  boost::format cmd_fmt{"%s copy -f %s --src-shared-blob-dir %s oci:%s docker-daemon:%s"};
  std::string cmd{boost::str(cmd_fmt % skopeo_bin_ % format % blobsRoot() % src_path.string() % tag)};
  return runCmd(cmd);
}

void SkopeoAppStore::purge(const AppEngine::Apps& app_shortlist) const {
  // purge Apps and make sure that we have only the shortlisted apps under root/apps directory
  std::unordered_set<std::string> blob_shortlist;
  purgeApps(app_shortlist, blob_shortlist);

  // parse all shortlisted App images' metadata in order to get a list/array of required/referenced blobs

  // generateAppBlobIndex(blob_shortlist);
  // purge blobs that are not in the list
  purgeBlobs(blob_shortlist);
}

void SkopeoAppStore::purgeApps(const AppEngine::Apps& app_shortlist,
                               std::unordered_set<std::string>& blob_shortlist) const {
  for (const auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(appsRoot()), {})) {
    if (!boost::filesystem::is_directory(entry)) {
      continue;
    }
    const std::string dir = entry.path().filename().native();
    auto foundAppIt = std::find_if(app_shortlist.begin(), app_shortlist.end(),
                                   [&dir](const AppEngine::App& app) { return dir == app.name; });

    if (foundAppIt == app_shortlist.end()) {
      // remove App dir tree since it's not found in the shortlist
      boost::filesystem::remove_all(entry.path());
      continue;
    }

    const auto& app{*foundAppIt};
    const Uri uri{Uri::parseUri(app.uri)};

    // iterate over `app` subdirectories/versions and remove those that doesn't match the specified version
    for (const auto& entry :
         boost::make_iterator_range(boost::filesystem::directory_iterator(appsRoot() / app.name), {})) {
      if (!boost::filesystem::is_directory(entry)) {
        LOG_WARNING << "Found file while expected an App version directory: " << entry.path().filename().native();
        continue;
      }

      const std::string app_version_dir = entry.path().filename().native();
      if (app_version_dir != uri.digest.hash()) {
        boost::filesystem::remove_all(dir);
        continue;
      }

      // add blobs of the shortlisted apps to the blob shortlist
      ComposeInfo compose{(entry.path() / "docker-compose.yml").string()};
      for (const auto& service : compose.getServices()) {
        const auto image = compose.getImage(service);
        const auto image_root{getAppImageRoot(app, image)};
        const auto image_manifest_desc{Utils::parseJSONFile(image_root / "index.json")};
        HashedDigest image_digest{image_manifest_desc["manifests"][0]["digest"].asString()};
        blob_shortlist.emplace(image_digest.hash());

        const auto image_manifest{Utils::parseJSONFile(blobsRoot() / "sha256" / image_digest.hash())};
        blob_shortlist.emplace(HashedDigest(image_manifest["config"]["digest"].asString()).hash());

        const auto image_layers{image_manifest["layers"]};
        for (Json::ValueConstIterator ii = image_layers.begin(); ii != image_layers.end(); ++ii) {
          if ((*ii).isObject() && (*ii).isMember("digest")) {
            const auto layer_digest{HashedDigest{(*ii)["digest"].asString()}};
            blob_shortlist.emplace(layer_digest.hash());
          } else {
            LOG_ERROR << "Invalid image manifest: " << ii.key().asString() << " -> " << *ii;
          }
        }
      }
    }
  }
}

void SkopeoAppStore::purgeBlobs(const std::unordered_set<std::string>& blob_shortlist) const {
  if (!boost::filesystem::exists(blobsRoot() / "sha256")) {
    return;
  }

  for (const auto& entry :
       boost::make_iterator_range(boost::filesystem::directory_iterator(blobsRoot() / "sha256"), {})) {
    if (boost::filesystem::is_directory(entry)) {
      continue;
    }

    const std::string blob_sha = entry.path().filename().native();
    if (blob_shortlist.end() == blob_shortlist.find(blob_sha)) {
      LOG_ERROR << "Removing blob: " << entry.path();
      boost::filesystem::remove_all(entry.path());
    }
  }
}

}  // namespace Docker
