#include "composeappengine.h"
#include "composeinfo.h"
#include "dockerclient.h"

#include <sys/statvfs.h>
#include <boost/process.hpp>

namespace Docker {

ComposeAppEngine::ComposeAppEngine(boost::filesystem::path root_dir, std::string compose_bin,
                                   Docker::DockerClient::Ptr docker_client, Docker::RegistryClient::Ptr registry_client)
    : root_{std::move(root_dir)},
      compose_{std::move(compose_bin)},
      docker_client_{std::move(docker_client)},
      registry_client_{std::move(registry_client)} {
  boost::filesystem::create_directories(root_);
}

bool ComposeAppEngine::fetch(const App& app) {
  boost::filesystem::create_directories(appRoot(app));
  if (download(app)) {
    LOG_INFO << "Validating compose file";
    if (cmd_streaming(compose_ + "config", app)) {
      LOG_INFO << "Pulling containers";
      return cmd_streaming(compose_ + "pull --no-parallel", app);
    }
  }
  return false;
}

bool ComposeAppEngine::install(const App& app) {
  boost::filesystem::ofstream flag_file(appRoot(app) / NeedStartFile);
  return cmd_streaming(compose_ + "up --remove-orphans --no-start", app);
}

bool ComposeAppEngine::run(const App& app) { return cmd_streaming(compose_ + "up --remove-orphans -d", app); }

void ComposeAppEngine::remove(const App& app) {
  if (cmd_streaming(compose_ + "down", app)) {
    boost::filesystem::remove_all(appRoot(app));
  } else {
    LOG_ERROR << "docker-compose was unable to bring down: " << appRoot(app);
  }
}

bool ComposeAppEngine::isRunning(const App& app) const {
  try {
    ComposeInfo info((appRoot(app) / ComposeFile).string());
    std::vector<Json::Value> services = info.getServices();
    if (services.empty()) {
      LOG_WARNING << "App: " << app.name << ", no services in docker file!";
      return false;
    }

    for (std::size_t i = 0; i < services.size(); i++) {
      std::string service = services[i].asString();
      std::string hash = info.getHash(services[i]);
      if (docker_client_->serviceRunning(app.name, service, hash)) {
        continue;
      }
      LOG_WARNING << "App: " << app.name << ", service: " << service << ", not running!";
      return false;
    }
    return true;
  } catch (...) {
    LOG_WARNING << "App: " << app.name << ", cant check if it is running!";
  }
  return false;
}

// Private implementation

struct Manifest : Json::Value {
  static constexpr const char* const Format{"application/vnd.oci.image.manifest.v1+json"};
  static constexpr const char* const Version{"v1"};

  explicit Manifest(const Json::Value& value = Json::Value()) : Json::Value(value) {
    auto manifest_version{(*this)["annotations"]["compose-app"].asString()};
    if (manifest_version.empty()) {
      throw std::runtime_error("Got invalid App manifest, missing a manifest version: " +
                               Utils::jsonToCanonicalStr(value));
    }
    if (Version != manifest_version) {
      throw std::runtime_error("Got unsupported App manifest version: " + Utils::jsonToCanonicalStr(value));
    }
  }

  std::string archiveDigest() const {
    auto digest{(*this)["layers"][0]["digest"].asString()};
    if (digest.empty()) {
      throw std::runtime_error("Got invalid App manifest, failed to extract App Archive digest from App manifest: " +
                               Utils::jsonToCanonicalStr(*this));
    }
    return digest;
  }

  size_t archiveSize() const {
    uint64_t arch_size{(*this)["layers"][0]["size"].asUInt64()};
    if (0 == arch_size || arch_size > std::numeric_limits<size_t>::max()) {
      throw std::runtime_error("Invalid size of App Archive is specified in App manifest: " +
                               Utils::jsonToCanonicalStr(*this));
    }

    return arch_size;
  }
};

// Utils::shell isn't interactive. The compose commands can take a few
// seconds to run, so we use boost::process:system to stream it to stdout/sterr
bool ComposeAppEngine::cmd_streaming(const std::string& cmd, const App& app) {
  LOG_DEBUG << "Running: " << cmd;
  int exit_code = boost::process::system(cmd, boost::process::start_dir = appRoot(app));
  return exit_code == 0;
}

std::pair<bool, std::string> ComposeAppEngine::cmd(const std::string& cmd) {
  std::string out_str;
  int exit_code = Utils::shell(cmd, &out_str, true);
  LOG_TRACE << "Command: " << cmd << "\n" << out_str;

  return {(exit_code == EXIT_SUCCESS), out_str};
}

bool ComposeAppEngine::download(const App& app) {
  bool result = false;

  try {
    LOG_DEBUG << app.name << ": downloading App from Registry: " << app.uri;

    Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
    Manifest manifest{registry_client_->getAppManifest(uri, Manifest::Format)};

    const std::string archive_file_name{uri.digest.shortHash() + '.' + app.name + ArchiveExt};
    Docker::Uri archive_uri{uri.createUri(manifest.archiveDigest())};

    uint64_t available_storage;
    if (checkAvailableStorageSpace(appRoot(app), available_storage)) {
      // assume that an extracted files total size is up to 10x larger than the archive size
      // 80% is a storage space watermark, we don't want to fill a storage volume above it
      auto need_storage = manifest.archiveSize() * 10;
      auto available_for_apps = static_cast<uint64_t>(available_storage * 0.8);
      if (need_storage > available_for_apps) {
        throw std::runtime_error("There is no sufficient storage space available to download App archive, available: " +
                                 std::to_string(available_for_apps) + " need: " + std::to_string(need_storage));
      }
    } else {
      LOG_WARNING << "Failed to get an available storage space, continuing with App archive download";
    }

    registry_client_->downloadBlob(archive_uri, appRoot(app) / archive_file_name, manifest.archiveSize());
    extractAppArchive(app, archive_file_name);

    result = true;
    LOG_DEBUG << app.name << ": App has been downloaded";
  } catch (const std::exception& exc) {
    LOG_ERROR << app.name << ": failed to download App from Registry: " << exc.what();
  }

  return result;
}

bool ComposeAppEngine::checkAvailableStorageSpace(const boost::filesystem::path& app_root,
                                                  uint64_t& out_available_size) {
  struct statvfs stat_buf {};
  const int stat_res = statvfs(app_root.c_str(), &stat_buf);
  if (stat_res < 0) {
    LOG_WARNING << "Unable to read filesystem statistics: error code " << stat_res;
    return false;
  }
  const uint64_t available_bytes = (static_cast<uint64_t>(stat_buf.f_bsize) * stat_buf.f_bavail);
  // 1 MB - reserved storage space, make sure a storage volume has at least 1 MB available
  // in addition to this preventive measure the function user/caller can add an additional use-case specific watermark
  const uint64_t reserved_bytes = 1 << 20;

  out_available_size = (available_bytes - reserved_bytes);
  return true;
}

void ComposeAppEngine::extractAppArchive(const App& app, const std::string& archive_file_name,
                                         bool delete_after_extraction) {
  if (!cmd_streaming("tar -xzf " + archive_file_name, app)) {
    throw std::runtime_error("Failed to extract the compose app archive: " + archive_file_name);
  }
  if (delete_after_extraction) {
    if (!cmd_streaming("rm -f " + archive_file_name, app)) {
      throw std::runtime_error("Failed to remove the compose app archive: " + archive_file_name);
    }
  }
}

}  // namespace Docker
