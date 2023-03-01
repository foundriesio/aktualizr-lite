#include "restorableappengine.h"

#include <sys/statvfs.h>
#include <limits>
#include <unordered_set>

#include <boost/algorithm/hex.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"
#include "docker/composeappengine.h"
#include "docker/composeinfo.h"
#include "exec.h"

namespace Docker {

class InsufficientSpaceError : public std::runtime_error {
 public:
  InsufficientSpaceError(const std::string& store, const std::string& path, const uint64_t& required,
                         const uint64_t& available)
      : std::runtime_error("Insufficient storage available; store: " + store + ", path: " + path +
                           ", required: " + std::to_string(required) + ", available: " + std::to_string(available)) {}
};

const std::string RestorableAppEngine::ComposeFile{"docker-compose.yml"};

RestorableAppEngine::StorageSpaceFunc RestorableAppEngine::GetDefStorageSpaceFunc(int watermark) {
  const int low_watermark_limit{LowWatermarkLimit};
  const int high_watermark_limit{HighWatermarkLimit};

  if (watermark < low_watermark_limit || watermark > high_watermark_limit) {
    throw std::invalid_argument(
        "Unsupported value of a storage watermark (sota.toml:pacman:storage_watermark); should be within [" +
        std::to_string(low_watermark_limit) + "," + std::to_string(high_watermark_limit) + "] range, got " +
        std::to_string(watermark));
  }

  return [watermark](const boost::filesystem::path& path) {
    boost::system::error_code ec;
    const boost::filesystem::space_info store_info{boost::filesystem::space(path, ec)};
    if (ec.failed()) {
      throw std::runtime_error("Failed to get an available storage size: " + ec.message());
    }

    boost::uintmax_t allowed_for_usage{static_cast<boost::uintmax_t>((float(watermark) / 100) * store_info.available)};
    return std::tuple<boost::uintmax_t, boost::uintmax_t>(
        store_info.available /* overall available */,
        allowed_for_usage /* available for usage taking into account a highe level watermark*/);
  };
}

RestorableAppEngine::RestorableAppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
                                         boost::filesystem::path docker_root,
                                         Docker::RegistryClient::Ptr registry_client,
                                         Docker::DockerClient::Ptr docker_client, std::string client,
                                         std::string docker_host, std::string compose_cmd,
                                         StorageSpaceFunc storage_space_func, ClientImageSrcFunc client_image_src_func,
                                         bool create_containers_if_install)
    : store_root_{std::move(store_root)},
      install_root_{std::move(install_root)},
      docker_root_{std::move(docker_root)},
      docker_and_skopeo_same_volume_{areDockerAndSkopeoOnTheSameVolume(store_root_, docker_root_)},
      client_{std::move(client)},
      docker_host_{std::move(docker_host)},
      compose_cmd_{std::move(compose_cmd)},
      registry_client_{std::move(registry_client)},
      docker_client_{std::move(docker_client)},
      storage_space_func_{std::move(storage_space_func)},
      client_image_src_func_{std::move(client_image_src_func)},
      create_containers_if_install_{create_containers_if_install} {
  boost::filesystem::create_directories(apps_root_);
  boost::filesystem::create_directories(blobs_root_);
}

AppEngine::Result RestorableAppEngine::fetch(const App& app) {
  Result res{false};
  boost::filesystem::path app_dir;
  try {
    const Uri uri{Uri::parseUri(app.uri)};
    app_dir = apps_root_ / uri.app / uri.digest.hash();
    const auto app_compose_file{app_dir / ComposeFile};

    if (!isAppFetched(app)) {
      LOG_INFO << app.name << ": downloading App from Registry: " << app.uri << " --> " << app_dir;
      pullApp(uri, app_dir);
    } else {
      LOG_INFO << app.name << ": App already fetched: " << app_dir;
    }

    // check App size
    checkAppUpdateSize(uri, app_dir);

    // Invoke download of App images unconditionally because `skopeo` is supposed
    // to skip already downloaded image blobs internally while performing `copy` command
    const auto images_dir{app_dir / "images"};
    LOG_DEBUG << app.name << ": downloading App images from Registry(ies): " << app.uri << " --> " << images_dir;
    pullAppImages(uri, app_compose_file, images_dir);
    res = true;
  } catch (const InsufficientSpaceError& exc) {
    res = {Result::ID::InsufficientSpace, exc.what()};
  } catch (const std::exception& exc) {
    res = {false, exc.what()};
  }

  if (!res) {
    if (boost::filesystem::exists(app_dir)) {
      boost::filesystem::remove_all(app_dir);
    }
  }
  return res;
}

AppEngine::Result RestorableAppEngine::verify(const App& app) {
  Result res{true};
  try {
    const Uri uri{Uri::parseUri(app.uri)};
    const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
    const Manifest manifest{Utils::parseJSONFile(app_dir / Manifest::Filename)};
    const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};
    TemporaryDirectory app_tmp_dir;

    // extract the compose file (i.e. docker-compose.yml) to a temporary directory to verify it.
    exec(boost::format{"tar -xzf %s %s"} % archive_full_path % ComposeFile, "no compose file found in archive",
         boost::process::start_dir = app_tmp_dir.Path());

    LOG_DEBUG << app.name << ": verifying App: " << app_dir << " --> " << app_tmp_dir.Path();
    exec(boost::format("%s config %s") % compose_cmd_ % "-q", "compose file verification failed",
         boost::process::start_dir = app_tmp_dir.Path());
  } catch (const std::exception& exc) {
    LOG_ERROR << "failed to verify App; app: " + app.name + "; uri: " + app.uri + "; err: " + exc.what();
    res = {false, exc.what()};
  }
  return res;
}

AppEngine::Result RestorableAppEngine::install(const App& app) {
  if (!create_containers_if_install_) {
    return installContainerless(app);
  }
  return installAndCreateOrRunContainers(app);
}

AppEngine::Result RestorableAppEngine::installContainerless(const App& app) {
  Result res{false};
  try {
    installAppAndImages(app);
    res = true;
  } catch (const std::exception& exc) {
    res = {false, exc.what()};
  }
  return res;
}

AppEngine::Result RestorableAppEngine::installAndCreateOrRunContainers(const App& app, bool run) {
  Result res{false};

  try {
    const auto app_install_root{installAppAndImages(app)};
    res = true;
  } catch (const std::exception& exc) {
    res = {false, exc.what()};
  }

  try {
    const auto app_install_root{install_root_ / app.name};
    // Make the docker store aware about the app images by invoking `docker compose pull` command.
    // The command fetches just image manifests since `installAppAndImages` injects all image data
    // to the docker store. The goal is to update the docker store map (`repositories.json`) between
    // image URIs and internal image represntation's hash.
    // If the pull fails then `ImagePullFailure` error is returned so the client can distnguish it from
    // the other image install/run errors and keep trying to install/run App containers.
    pullComposeAppImages(compose_cmd_, app_install_root);
  } catch (const std::exception& exc) {
    return {Result::ID::ImagePullFailure, exc.what()};
  }

  try {
    const auto app_install_root{install_root_ / app.name};
    const std::string flags{run ? "--remove-orphans -d" : "--remove-orphans --no-start"};
    startComposeApp(compose_cmd_, app_install_root, flags);
    res = true;
  } catch (const std::exception& exc) {
    res = {false, exc.what()};
  }

  if (!res) {
    return res;
  }

  try {
    if (!areContainersCreated(app, (install_root_ / app.name / ComposeFile).string(), docker_client_)) {
      const std::string action{run ? "started" : "created"};
      res = {false, "App containers haven't been " + action};
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "failed to check whether containers have been created: " << exc.what();
    // if we fail (exception) to check whether containers have been created is doesn't mean
    // that installation was not succesful, just assume that the App creation succeeded if
    // `docker-compose up` returns EXIT_SUCCESS
  }

  return res;
}

AppEngine::Result RestorableAppEngine::run(const App& app) { return installAndCreateOrRunContainers(app, true); }

void RestorableAppEngine::stop(const App& app) {
  try {
    const auto app_install_dir{install_root_ / app.name};

    // just installed app are removed, the restorable store Apps will be removed by means of prune() call
    stopComposeApp(compose_cmd_, app_install_dir);
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", failed to remove: " << exc.what();
  }
}

void RestorableAppEngine::remove(const App& app) {
  try {
    const auto app_install_dir{install_root_ / app.name};

    // just installed app are removed, the restorable store Apps will be removed by means of prune() call
    stopComposeApp(compose_cmd_, app_install_dir);
    boost::filesystem::remove_all(app_install_dir);

  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", failed to remove: " << exc.what();
  }
}

bool RestorableAppEngine::isFetched(const App& app) const {
  bool res{false};
  try {
    res = isAppFetched(app);
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", cannot check whether App is fetched: " << exc.what();
  }
  return res;
}

bool RestorableAppEngine::isRunning(const App& app) const {
  bool res{false};

  try {
    res = isAppFetched(app) && isAppInstalled(app) &&
          isRunning(app, (install_root_ / app.name / ComposeFile).string(), docker_client_);
  } catch (const std::exception& exc) {
    LOG_WARNING << "App: " << app.name << ", cannot check whether App is running: " << exc.what();
  }

  return res;
}

Json::Value RestorableAppEngine::getRunningAppsInfo() const {
  Json::Value apps;
  try {
    apps = docker_client_->getRunningApps(nullptr);
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get an info about running containers: " << exc.what();
  }

  return apps;
}

void RestorableAppEngine::prune(const Apps& app_shortlist) {
  std::unordered_set<std::string> blob_shortlist;
  bool prune_docker_store{false};

  for (const auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(apps_root_), {})) {
    if (!boost::filesystem::is_directory(entry)) {
      continue;
    }
    const std::string dir = entry.path().filename().native();
    auto foundAppIt = std::find_if(app_shortlist.begin(), app_shortlist.end(),
                                   [&dir](const AppEngine::App& app) { return dir == app.name; });

    if (foundAppIt == app_shortlist.end()) {
      // remove App dir tree since it's not found in the shortlist
      boost::filesystem::remove_all(entry.path());
      LOG_INFO << "Removing App dir: " << entry.path();
      prune_docker_store = true;
      continue;
    }

    const auto& app{*foundAppIt};
    const Uri uri{Uri::parseUri(app.uri)};

    // iterate over `app` subdirectories/versions and remove those that doesn't match the specified version
    const auto app_dir{apps_root_ / uri.app};

    for (const auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(app_dir), {})) {
      if (!boost::filesystem::is_directory(entry)) {
        LOG_WARNING << "Found file while expected an App version directory: " << entry.path().filename().native();
        continue;
      }

      const std::string app_version_dir = entry.path().filename().native();
      if (app_version_dir != uri.digest.hash()) {
        LOG_INFO << "Removing App version dir: " << entry.path();
        boost::filesystem::remove_all(entry.path());
        prune_docker_store = true;
        continue;
      }

      // add blobs of the shortlisted apps to the blob shortlist
      ComposeInfo compose{(entry.path() / ComposeFile).string()};
      for (const auto& service : compose.getServices()) {
        const auto image = compose.getImage(service);
        const Uri image_uri{Uri::parseUri(image, false)};
        const auto image_root{app_dir / app_version_dir / "images" / image_uri.registryHostname / image_uri.repo /
                              image_uri.digest.hash()};

        const auto index_manifest{image_root / "index.json"};
        if (!boost::filesystem::exists(index_manifest)) {
          LOG_WARNING << "Failed to find an index manifest of App image: " << image << ", removing its directory";
          boost::filesystem::remove_all(image_root);
          continue;
        }

        const auto image_manifest_desc{Utils::parseJSONFile(index_manifest)};
        HashedDigest image_digest{image_manifest_desc["manifests"][0]["digest"].asString()};
        blob_shortlist.emplace(image_digest.hash());

        const auto image_manifest{Utils::parseJSONFile(blobs_root_ / "sha256" / image_digest.hash())};
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

  // prune blobs
  if (!boost::filesystem::exists(blobs_root_ / "sha256")) {
    return;
  }

  for (const auto& entry :
       boost::make_iterator_range(boost::filesystem::directory_iterator(blobs_root_ / "sha256"), {})) {
    if (boost::filesystem::is_directory(entry)) {
      continue;
    }

    const std::string blob_sha = entry.path().filename().native();
    if (blob_shortlist.end() == blob_shortlist.find(blob_sha)) {
      LOG_INFO << "Removing blob: " << entry.path();
      boost::filesystem::remove_all(entry.path());
      prune_docker_store = true;
    }
  }

  // prune docker store
  if (prune_docker_store) {
    ComposeAppEngine::pruneDockerStore(*docker_client_);
  }
}

// protected & private implementation

void RestorableAppEngine::pullApp(const Uri& uri, const boost::filesystem::path& app_dir) {
  boost::filesystem::create_directories(app_dir);

  const std::string manifest_str{registry_client_->getAppManifest(uri, Manifest::Format)};
  const Manifest manifest{manifest_str};
  Docker::Uri archive_uri{uri.createUri(HashedDigest(manifest.archiveDigest()))};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  boost::system::error_code ec;
  boost::filesystem::space_info fs_storage_info{boost::filesystem::space(app_dir, ec)};
  if (ec.failed()) {
    LOG_WARNING << "Failed to get an available storage size: " << ec.message();
  } else {
    // assume that an extracted files total size is up to 3x larger than the archive size
    // 80% is a storage space watermark, we don't want to fill a storage volume above it
    auto need_storage = manifest.archiveSize() * 3;
    auto available = static_cast<boost::uintmax_t>(fs_storage_info.available * 0.8);
    if (need_storage > available) {
      throw std::runtime_error("There is no sufficient storage space available to download App archive, available: " +
                               std::to_string(available) + " need: " + std::to_string(need_storage));
    }
  }

  registry_client_->downloadBlob(archive_uri, archive_full_path, manifest.archiveSize());
  Utils::writeFile(app_dir / Manifest::Filename, manifest_str);
  Utils::writeFile(app_dir / "uri", uri.registryHostname + "/" + uri.repo + "@" + uri.digest());
  // extract docker-compose.yml, temporal hack, we don't need to extract it
  exec(boost::format{"tar -xzf %s %s"} % archive_full_path % ComposeFile, "no compose file found in archive",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::checkAppUpdateSize(const Uri& uri, const boost::filesystem::path& app_dir) const {
  const Manifest manifest{Utils::parseJSONFile(app_dir / Manifest::Filename)};
  const auto arch{docker_client_->arch()};
  if (arch.empty()) {
    LOG_WARNING << "Failed to get an info about a system architecture";
    return;
  }

  const auto layers_manifest{manifest.layersManifest(arch)};
  if (!layers_manifest.isObject()) {
    LOG_WARNING << "App layers' manifest is missing, skip checking an App update size";
    return;
  }

  if (!(layers_manifest.isMember("digest") && layers_manifest["digest"].isString())) {
    throw std::invalid_argument("Got invalid layers manifest, missing or incorrect `digest` field");
  }

  if (!(layers_manifest.isMember("size") && layers_manifest["size"].isInt64())) {
    throw std::invalid_argument("Got invalid layers manifest, missing or incorrect `size` field");
  }

  const Docker::Uri layers_manifest_uri{uri.createUri(HashedDigest(layers_manifest["digest"].asString()))};
  const std::int64_t layers_manifest_size{layers_manifest["size"].asInt64()};

  const std::string man_str{
      registry_client_->getAppManifest(layers_manifest_uri, Manifest::IndexFormat, layers_manifest_size)};
  const auto man{Utils::parseJSON(man_str)};

  LOG_INFO << "Checking for App's new layers...";
  uint64_t skopeo_total_update_size{getAppUpdateSize(man["layers"], blobs_root_ / "sha256")};
  const uint32_t average_compression_ratio{5} /* gzip layer compression ratio */;
  uint64_t docker_total_update_size{
      getDockerStoreSizeForAppUpdate(skopeo_total_update_size, average_compression_ratio)};

  LOG_INFO << "Checking if there is sufficient amount of storage available for App update...";
  checkAvailableStorageInStores(uri.app, skopeo_total_update_size, docker_total_update_size);
}

void RestorableAppEngine::pullAppImages(const Uri& app_uri, const boost::filesystem::path& app_compose_file,
                                        const boost::filesystem::path& dst_dir) {
  // REGISTRY_AUTH_FILE env. var. must be set and point to the docker's `config.json` (e.g.
  // /usr/lib/docker/config.json)`
  // {
  //  "credHelpers": {
  //    "hub.foundries.io": "fio-helper"
  //  }
  // }
  //
  // `"hub.foundries.io": "fio-helper"` implies that there is `/usr/bin/docker-credential-fio-helper` which returns
  // creds access to customer specific registry can be provided in the same way, i.e. defining the registry specific
  // cred helper.
  boost::filesystem::create_directories(dst_dir);

  const auto compose{ComposeInfo(app_compose_file.string())};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);

    const Uri uri{Uri::parseUri(image_uri, false)};
    const auto image_dir{dst_dir / uri.registryHostname / uri.repo / uri.digest.hash()};

    LOG_INFO << uri.app << ": downloading image from Registry if missing: " << image_uri << " --> " << image_dir;
    const std::string image_src{client_image_src_func_(app_uri, image_uri)};
    pullImage(client_, image_src, image_dir, blobs_root_);
  }
}

boost::filesystem::path RestorableAppEngine::installAppAndImages(const App& app) {
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  auto app_install_dir{install_root_ / app.name};
  LOG_DEBUG << app.name << ": installing App: " << app_dir << " --> " << app_install_dir;
  installApp(app_dir, app_install_dir);
  LOG_DEBUG << app.name << ": verifying App: " << app_install_dir;
  verifyComposeApp(compose_cmd_, app_install_dir);
  LOG_DEBUG << app.name << ": installing App images: " << app_dir << " --> docker-daemon://";
  installAppImages(app_dir);
  return app_install_dir;
}

void RestorableAppEngine::installApp(const boost::filesystem::path& app_dir, const boost::filesystem::path& dst_dir) {
  const Manifest manifest{Utils::parseJSONFile(app_dir / Manifest::Filename)};
  const auto archive_full_path{app_dir / (HashedDigest(manifest.archiveDigest()).hash() + Manifest::ArchiveExt)};

  boost::filesystem::create_directories(dst_dir);
  exec(boost::format{"tar --overwrite -xzf %s"} % archive_full_path.string(), "failed to install Compose App",
       boost::process::start_dir = dst_dir);
}

void RestorableAppEngine::installAppImages(const boost::filesystem::path& app_dir) {
  const auto compose{ComposeInfo((app_dir / ComposeFile).string())};
  for (const auto& service : compose.getServices()) {
    const auto image_uri = compose.getImage(service);
    const Uri uri{Uri::parseUri(image_uri, false)};
    const std::string tag{uri.registryHostname + '/' + uri.repo + ':' + uri.digest.shortHash()};
    const auto image_dir{app_dir / "images" / uri.registryHostname / uri.repo / uri.digest.hash()};
    installImage(client_, image_dir, blobs_root_, docker_host_, tag);
  }
}

bool RestorableAppEngine::isAppFetched(const App& app) const {
  bool res{false};
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};

  do {
    if (!boost::filesystem::exists(app_dir)) {
      LOG_DEBUG << app.name << ": missing App dir: " << app_dir;
      break;
    }

    const auto manifest_file{app_dir / Manifest::Filename};
    if (!boost::filesystem::exists(manifest_file)) {
      LOG_DEBUG << app.name << ": missing App manifest: " << manifest_file;
      break;
    }

    const auto manifest_hash{getContentHash(manifest_file)};
    if (manifest_hash != uri.digest.hash()) {
      LOG_DEBUG << app.name << ": App manifest hash mismatch; actual: " << manifest_hash
                << "; expected: " << uri.digest.hash();
      break;
    }

    const Manifest manifest{Utils::parseJSONFile(manifest_file)};

    // verify App archive/blob hash
    const auto archive_manifest_hash{HashedDigest(manifest.archiveDigest()).hash()};
    const auto archive_full_path{app_dir / (archive_manifest_hash + Manifest::ArchiveExt)};
    if (!boost::filesystem::exists(archive_full_path)) {
      LOG_DEBUG << app.name << ": missing App archive: " << archive_full_path;
      break;
    }

    // we assume that a compose App blob is relatively small so we can just read it all into RAM
    const auto app_arch_hash{getContentHash(archive_full_path)};
    if (app_arch_hash != archive_manifest_hash) {
      LOG_DEBUG << app.name << ": App archive hash mismatch; actual: " << app_arch_hash
                << "; defined in manifest: " << archive_manifest_hash;
      break;
    }

    // TODO: we actually should extract a compose file from the app archive, otherwise
    // it won't be merkle tree like verification since this compose file can be hacked
    // because it doesn't have any hash to be verified for.
    // As an alternative we might consider adding a new attribute to an App root manifest that denotes a hash of a
    // compose file. Or, a compose yaml should be excluded from an App's archive and considered as App config that is
    // referenced by ["config"]["digest"] element of an App manifest. (currently this digest is equal to the hash of
    // empty string)
    const auto compose_file{app_dir / ComposeFile};
    if (!boost::filesystem::exists(compose_file)) {
      LOG_DEBUG << app.name << ": missing App compose file: " << compose_file;
      break;
    }

    // No need to check hashes of a Merkle tree of each App image since skopeo does it internally within in the `skopeo
    // copy` command. While the above statement is true there is still a need in traversing App's merkle tree at the
    // phase of checking whether Apps are in sync with Target because `skopeo copy` is invoked only if the check detects
    // any missing piece of App. If the given check doesn't detect any missing part of App then the fetch&install phase
    // never happens and `skopeo copy` is never invoked. So, let's do in areAppImagesFetched() method.

    res = areAppImagesFetched(app);
  } while (false);

  return res;
}

bool RestorableAppEngine::areAppImagesFetched(const App& app) const {
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  const auto compose_file{app_dir / ComposeFile};

  ComposeInfo compose{compose_file.string()};
  for (const auto& service : compose.getServices()) {
    const auto image = compose.getImage(service);
    const Uri image_uri{Uri::parseUri(image, false)};
    const auto image_root{app_dir / "images" / image_uri.registryHostname / image_uri.repo / image_uri.digest.hash()};

    const auto index_manifest{image_root / "index.json"};
    if (!boost::filesystem::exists(index_manifest)) {
      LOG_DEBUG << app.name << ": missing index manifest of App image; image: " << image
                << "; index: " << index_manifest;
      return false;
    }

    // Unfortunately `skopeo` trims an index/list image manifest by removing from it each image manifests that
    // doesn't match the current architecture. Therefore, it's not possible or doesn't make sense to compare
    // the image digest (image_uri.digest.hash()) with a hash of actuall content of index.json.
    // TODO: consider patching skopeo or adding cli param to make it store an intact image index manifest.

    const auto manifest_desc{Utils::parseJSONFile(index_manifest)};
    HashedDigest manifest_digest{manifest_desc["manifests"][0]["digest"].asString()};

    const auto manifest_file{blobs_root_ / "sha256" / manifest_digest.hash()};
    if (!boost::filesystem::exists(manifest_file)) {
      LOG_DEBUG << app.name << ": missing App image manifest; image: " << image << "; manifest: " << manifest_file;
      return false;
    }

    const auto manifest_hash{getContentHash(manifest_file)};
    if (manifest_hash != manifest_digest.hash()) {
      LOG_DEBUG << app.name << ": App image manifest hash mismatch; actual: " << manifest_hash
                << "; expected: " << manifest_digest.hash();
      return false;
    }

    const auto manifest{Utils::parseJSONFile(blobs_root_ / "sha256" / manifest_digest.hash())};

    // check image config file/blob
    const auto config_digest{HashedDigest(manifest["config"]["digest"].asString())};
    const auto config_file{blobs_root_ / "sha256" / config_digest.hash()};

    if (!boost::filesystem::exists(config_file)) {
      LOG_DEBUG << app.name << ": missing App image config file; image: " << image << "; manifest: " << config_file;
      return false;
    }

    const auto config_hash{getContentHash(config_file)};
    if (config_hash != config_digest.hash()) {
      LOG_DEBUG << app.name << ": App image config hash mismatch; actual: " << config_hash
                << "; expected: " << config_digest.hash();
      return false;
    }

    // check layers, just check blobs' size since generation of their hashes might consumes
    // too much CPU for a given device ???
    const auto layers{manifest["layers"]};
    for (Json::ValueConstIterator ii = layers.begin(); ii != layers.end(); ++ii) {
      if ((*ii).isObject() && (*ii).isMember("digest") && (*ii).isMember("size")) {
        const auto layer_digest{HashedDigest{(*ii)["digest"].asString()}};
        const auto layer_size{(*ii)["size"].asInt64()};
        const auto blob_path{blobs_root_ / "sha256" / layer_digest.hash()};
        if (!boost::filesystem::exists(blob_path)) {
          LOG_DEBUG << app.name << ": missing App image blob; image: " << image << "; blob: " << blob_path;
          return false;
        }
        const auto blob_size{boost::filesystem::file_size(blob_path)};
        if (blob_size != layer_size) {
          LOG_DEBUG << app.name << ": App image blob size mismatch; blob: " << blob_path << "; actual: " << blob_size
                    << "; expected: " << layer_size;
          // `skopeo copy` gets crazy if one or more blobs are invalid/altered/broken, it just simply fails
          // instead of refetching it (another candidate for patching),
          // so, we just remove the broken blob.
          boost::filesystem::remove(blob_path);
          return false;
        }

      } else {
        LOG_ERROR << app.name << ": invalid image manifest: " << ii.key().asString() << " -> " << *ii;
        return false;
      }
    }
  }

  return true;
}

bool RestorableAppEngine::isAppInstalled(const App& app) const {
  bool res{false};
  const Uri uri{Uri::parseUri(app.uri)};
  const auto app_dir{apps_root_ / uri.app / uri.digest.hash()};
  const auto app_install_dir{install_root_ / app.name};

  do {
    const auto manifest_file{app_dir / Manifest::Filename};
    const Manifest manifest{Utils::parseJSONFile(manifest_file)};
    const auto archive_manifest_hash{HashedDigest(manifest.archiveDigest()).hash()};
    const auto archive_full_path{app_dir / (archive_manifest_hash + Manifest::ArchiveExt)};

    exec(boost::format{"tar -xzf %s %s"} % archive_full_path % ComposeFile,
         app.name + ": failed to extract a compose file from the compose app archive",
         boost::this_process::environment(), boost::process::start_dir = app_dir);

    const auto compose_file_str = Utils::readFile(app_dir / ComposeFile);
    const auto compose_file_hash =
        boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(compose_file_str)));
    const auto installed_compose_file_str = Utils::readFile(app_install_dir / ComposeFile);
    const auto installed_compose_file_hash =
        boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(installed_compose_file_str)));

    if (compose_file_hash != installed_compose_file_hash) {
      LOG_DEBUG << app.name << "; a compose file hash mismatch; installed: " << installed_compose_file_hash
                << "fetched: " << compose_file_hash;
      break;
    }
    // TODO: check whether docker store has all App images

    res = true;
  } while (false);

  return res;
}

bool RestorableAppEngine::isRunning(const App& app, const std::string& compose_file,
                                    const Docker::DockerClient::Ptr& docker_client) {
  return checkAppContainers(app, compose_file, docker_client);
}

bool RestorableAppEngine::areContainersCreated(const App& app, const std::string& compose_file,
                                               const Docker::DockerClient::Ptr& docker_client) {
  return checkAppContainers(app, compose_file, docker_client, false);
}

bool RestorableAppEngine::checkAppContainers(const App& app, const std::string& compose_file,
                                             const Docker::DockerClient::Ptr& docker_client, bool check_state) {
  bool result{true};
  ComposeInfo compose{compose_file};
  std::vector<Json::Value> services = compose.getServices();

  if (services.empty()) {
    throw std::runtime_error("No services found in App's compose file");
  }

  Json::Value containers;
  docker_client->getContainers(containers);

  for (std::size_t i = 0; i < services.size(); i++) {
    std::string service = services[i].asString();
    std::string hash = compose.getHash(services[i]);
    const auto container_state{docker_client->getContainerState(containers, app.name, service, hash)};
    if (std::get<0>(container_state) /* container exists */ &&
        (!check_state || std::get<1>(container_state) != "created")) {
      continue;
    }
    LOG_WARNING << "App: " << app.name << ", service: " << service << ", hash: " << hash << ", not running!";
    result = false;
    break;
  }

  return result;
}

// static methods to manage image data and Compose App

void RestorableAppEngine::pullImage(const std::string& client, const std::string& src,
                                    const boost::filesystem::path& dst_dir,
                                    const boost::filesystem::path& shared_blob_dir, const std::string& format) {
  boost::filesystem::create_directories(dst_dir);
  exec(boost::format{"%s copy -f %s --dest-shared-blob-dir %s %s oci:%s"} % client % format % shared_blob_dir.string() %
           src % dst_dir.string(),
       "failed to pull image", boost::this_process::environment());
}

void RestorableAppEngine::installImage(const std::string& client, const boost::filesystem::path& image_dir,
                                       const boost::filesystem::path& shared_blob_dir, const std::string& docker_host,
                                       const std::string& tag, const std::string& format) {
  exec(boost::format{"%s copy -f %s --dest-daemon-host %s --src-shared-blob-dir %s oci:%s docker-daemon:%s"} % client %
           format % docker_host % shared_blob_dir.string() % image_dir.string() % tag,
       "failed to install image", boost::this_process::environment());
}

void RestorableAppEngine::verifyComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir) {
  exec(boost::format{"%s config"} % compose_cmd, "Compose App verification failed",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::pullComposeAppImages(const std::string& compose_cmd, const boost::filesystem::path& app_dir,
                                               const std::string& flags) {
  exec(boost::format{"%s pull %s"} % compose_cmd % flags, "failed to pull Compose App images",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::startComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir,
                                          const std::string& flags) {
  exec(boost::format{"%s up %s"} % compose_cmd % flags, "failed to bring Compose App up",
       boost::process::start_dir = app_dir);
}

void RestorableAppEngine::stopComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir) {
  exec(boost::format{"%s down"} % compose_cmd, "failed to bring Compose App down", boost::process::start_dir = app_dir);
}

std::string RestorableAppEngine::getContentHash(const boost::filesystem::path& path) {
  const auto content{Utils::readFile(path)};
  return boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(content)));
}

uint64_t RestorableAppEngine::getAppUpdateSize(const Json::Value& app_layers, const boost::filesystem::path& blob_dir) {
  std::unordered_set<std::string> store_blobs;

  if (boost::filesystem::exists(blob_dir)) {
    for (const boost::filesystem::directory_entry& entry : boost::filesystem::directory_iterator(blob_dir)) {
      store_blobs.emplace(entry.path().filename().string());
    }
  }

  // It can happen that one or more currently stored blobs/layers are not needed for the new App
  // and they will be purged after an update completion therefore we actually will need less than
  // `total_update_size` additional storage to accomodate a new App. Moreover, a new App even might
  // occupy even less space than the current App.
  // But, during an update process there is a moment at which a sum of both the current's and new App's layers
  // are stored on storage, thus we need to make sure that underlying storage can accomodate the sum of the Apps'
  // layers set/list.

  uint64_t skopeo_total_update_size{0};

  for (Json::ValueConstIterator ii = app_layers.begin(); ii != app_layers.end(); ++ii) {
    const HashedDigest digest{(*ii)["digest"].asString()};
    if (store_blobs.count(digest.hash()) == 0) {
      // According to the spec the `size` field must be int64
      // https://github.com/opencontainers/image-spec/blob/main/descriptor.md#properties
      const auto size_obj{(*ii)["size"]};
      if (!size_obj.isInt64()) {
        throw std::range_error("Invalid value of a layer size, must be int64, got: " + size_obj.asString());
      }

      const std::int64_t size{size_obj.asInt64()};
      if (size < 0) {
        throw std::range_error("Invalid value of a layer size, must be > 0, got: " + std::to_string(size));
      }

      const uint64_t new_total_update_size = skopeo_total_update_size + size;
      if (new_total_update_size < skopeo_total_update_size || new_total_update_size < size) {
        throw std::overflow_error("Sum of layer sizes exceeded the maximum allowed value: " +
                                  std::to_string(std::numeric_limits<uint64_t>::max()));
      }

      LOG_INFO << "\t" << digest.hash() << " -> missing; to be downloaded; size: " << size;
      skopeo_total_update_size = new_total_update_size;
    } else {
      LOG_INFO << "\t" << digest.hash() << " -> exists";
    }
  }
  return skopeo_total_update_size;
}

uint64_t RestorableAppEngine::getDockerStoreSizeForAppUpdate(const uint64_t& compressed_update_size,
                                                             uint32_t average_compression_ratio) {
  // approximate an amount of storage required to accomodate the App update in the docker store
  uint64_t docker_total_update_size{0};
  // update size in uncompressed format (docker data root), skopeo_total_update_size * average_compression_ratio
  if (__builtin_mul_overflow(compressed_update_size, average_compression_ratio, &docker_total_update_size)) {
    throw std::overflow_error("Docker total update size exceeds the maximum allowed value: " +
                              std::to_string(std::numeric_limits<uint64_t>::max()));
  }
  return docker_total_update_size;
}

void RestorableAppEngine::checkAvailableStorageInStores(const std::string& app_name,
                                                        const uint64_t& skopeo_required_storage,
                                                        const uint64_t& docker_required_storage) const {
  auto checkRoomInStore = [&](const std::string& store_name, const uint64_t& required_storage,
                              const boost::filesystem::path& store_path) {
    boost::uintmax_t capacity;
    boost::uintmax_t available;

    std::tie(capacity, available) = storage_space_func_(store_path);
    LOG_INFO << app_name << " -> " << store_name << " store total update size: " << required_storage
             << " bytes; available: " << available << " (out of " << capacity << ") "
             << ", path: " << store_path.string();
    if (required_storage > available) {
      throw InsufficientSpaceError(store_name, store_path.string(), required_storage, available);
    }
  };

  checkRoomInStore("skopeo", skopeo_required_storage, store_root_);
  checkRoomInStore("docker", docker_required_storage, docker_root_);

  if (docker_and_skopeo_same_volume_) {
    const uint64_t combined_total_required_size{skopeo_required_storage + docker_required_storage};
    if (combined_total_required_size < skopeo_required_storage ||
        combined_total_required_size < docker_required_storage) {
      throw std::overflow_error("Sum of skopeo and docker update sizes exceeds the maximum allowed value: " +
                                std::to_string(std::numeric_limits<uint64_t>::max()));
    }
    checkRoomInStore("skopeo & docker", combined_total_required_size, store_root_);
  }
}

bool RestorableAppEngine::areDockerAndSkopeoOnTheSameVolume(const boost::filesystem::path& skopeo_path,
                                                            const boost::filesystem::path& docker_path) {
  const auto skopeoVolumeID{getPathVolumeID(skopeo_path.parent_path())};
  if (!std::get<1>(skopeoVolumeID)) {
    LOG_WARNING << "Failed to obtain an ID of a skopeo store volume; path: " << skopeo_path
                << ", err: " << strerror(errno);
  }
  const auto dockerVolumeID{getPathVolumeID(docker_path.parent_path())};
  if (!std::get<1>(dockerVolumeID)) {
    LOG_WARNING << "Failed to obtain an ID of a docker store volume; path: " << docker_path
                << ", err: " << strerror(errno);
  }
  // we assume that a docker data root and a skopeo store are located on the same volume in case of a failure to obtain
  // a volume ID
  return std::get<0>(skopeoVolumeID) == std::get<0>(dockerVolumeID);
}

std::tuple<uint64_t, bool> RestorableAppEngine::getPathVolumeID(const boost::filesystem::path& path) {
  struct statvfs stvfsbuf {};
  const int stat_res = statvfs(path.c_str(), &stvfsbuf);
  if (stat_res < 0) {
    return {0, false};
  } else {
    return {stvfsbuf.f_fsid, true};
  }
}

}  // namespace Docker
