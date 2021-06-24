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
      registry_client_{std::move(registry_client)} {}

bool ComposeAppEngine::fetch(const App& app) {
  boost::filesystem::create_directories(appRoot(app) / MetaDir);

  bool result = false;

  try {
    AppState state(app, appRoot(app), true);

    if (!download(app)) {
      state.setState(AppState::State::kDownloadFailed);
      return result;
    }
    state.setState(AppState::State::kDownloaded);

    if (!verify(app)) {
      state.setState(AppState::State::kVerifyFailed);
      return result;
    }
    state.setState(AppState::State::kVerified);

    if (!pullImages(app)) {
      state.setState(AppState::State::kPullFailed);
      return result;
    }
    state.setState(AppState::State::kPulled);

    result = true;
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get/set App state, fallback to forced fetch: " << exc.what();

    if (download(app) && verify(app) && pullImages(app)) {
      result = true;
    }
  }

  return result;
}

bool ComposeAppEngine::install(const App& app) {
  bool result = false;

  if (!boost::filesystem::exists(appRoot(app))) {
    LOG_ERROR << "App dir doesn't exist, cannot install App that hasn't been fetched";
    return result;
  }

  try {
    AppState state(app, appRoot(app));

    switch (state()) {
      case AppState::State::kPulled:
      case AppState::State::kInstallFail: {
        if (!installApp(app)) {
          state.setState(AppState::State::kInstallFail);
          break;
        }
        state.setState(AppState::State::kInstalled);
      }
      case AppState::State::kInstalled:
      case AppState::State::kStarted:
        result = true;
        break;
      default:
        LOG_ERROR << "Cannot install App that hasn't been fetched";
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get/set App state, fallback to forced install: " << exc.what();

    if (installApp(app)) {
      result = true;
    }
  }
  return result;
}

bool ComposeAppEngine::run(const App& app) {
  bool result = false;

  if (!boost::filesystem::exists(appRoot(app))) {
    LOG_ERROR << "App dir doesn't exist, cannot start App that hasn't been fetched";
    return result;
  }

  try {
    AppState state(app, appRoot(app));

    switch (state()) {
      case AppState::State::kPulled:
      case AppState::State::kInstalled:
      case AppState::State::kInstallFail:
      case AppState::State::kStartFailed: {
        if (!start(app)) {
          state.setState(AppState::State::kStartFailed);
          break;
        }
        state.setState(AppState::State::kStarted);
      }
      case AppState::State::kStarted:
        result = true;
        break;
      default:
        LOG_ERROR << "Cannot start App that hasn't been fetched";
    }
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get/set App state, fallback to forced run: " << exc.what();

    if (start(app)) {
      result = true;
    }
  }
  return result;
}

void ComposeAppEngine::remove(const App& app) {
  if (cmd_streaming(compose_ + "down", app)) {
    boost::filesystem::remove_all(appRoot(app));
  } else {
    LOG_ERROR << "docker-compose was unable to bring down: " << appRoot(app);
  }
}

bool ComposeAppEngine::isRunning(const App& app) const {
  if (!boost::filesystem::exists(appRoot(app))) {
    return false;
  }

  bool started_state = false;
  try {
    AppState state(app, appRoot(app));
    started_state = state() == AppState::State::kStarted;
  } catch (const std::exception& exc) {
    LOG_DEBUG << "Failed to get/set App state, fallback to checking the dockerd state: " << exc.what();
    started_state = true;
  }

  // let's fallback or do double check
  try {
    ComposeInfo info((appRoot(app) / ComposeFile).string());
    std::vector<Json::Value> services = info.getServices();
    if (services.empty()) {
      LOG_WARNING << "App: " << app.name << ", no services in docker file!";
      return false;
    }

    Json::Value containers;
    docker_client_->getContainers(containers);

    for (std::size_t i = 0; i < services.size(); i++) {
      std::string service = services[i].asString();
      std::string hash = info.getHash(services[i]);
      if (docker_client_->isRunning(containers, app.name, service, hash)) {
        continue;
      }
      LOG_WARNING << "App: " << app.name << ", service: " << service << ", hash: " << hash << ", not running!";
      return false;
    }
    return started_state;
  } catch (...) {
    LOG_WARNING << "App: " << app.name << ", cant check if it is running!";
  }

  return false;
}

Json::Value ComposeAppEngine::getRunningAppsInfo() const {
  Json::Value apps;
  try {
    Json::Value containers;
    docker_client_->getContainers(containers);

    for (Json::ValueIterator ii = containers.begin(); ii != containers.end(); ++ii) {
      Json::Value val = *ii;

      std::string app_name = val["Labels"]["com.docker.compose.project"].asString();
      if (app_name.empty()) {
        continue;
      }

      if (!apps.isMember(app_name)) {
        App app{app_name, ""};
        AppState state(app, appRoot(app));
        app.uri = state.version();
        apps[app.name]["uri"] = app.uri;
        apps[app.name]["state"] = state.toStr();
      }

      std::string service = val["Labels"]["com.docker.compose.service"].asString();
      std::string hash = val["Labels"]["io.compose-spec.config-hash"].asString();
      std::string image = val["Image"].asString();
      std::string state = val["State"].asString();
      std::string status = val["Status"].asString();

      apps[app_name]["services"][service]["hash"] = hash;
      apps[app_name]["services"][service]["image"] = image;
      apps[app_name]["services"][service]["state"] = state;
      apps[app_name]["services"][service]["status"] = status;
    }

  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get an info about running containers: " << exc.what();
  }

  return apps;
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

bool ComposeAppEngine::verify(const App& app) {
  LOG_INFO << "Validating compose file";
  const auto result = cmd_streaming(compose_ + "config", app);
  return result;
}

bool ComposeAppEngine::pullImages(const App& app) {
  LOG_INFO << "Pulling containers";
  const auto result = cmd_streaming(compose_ + "pull --no-parallel", app);
  return result;
}

bool ComposeAppEngine::installApp(const App& app) {
  LOG_INFO << "Installing App";
  const auto result = cmd_streaming(compose_ + "up --remove-orphans --no-start", app);
  return result;
}

bool ComposeAppEngine::start(const App& app) {
  LOG_INFO << "Starting App";
  const auto result = cmd_streaming(compose_ + "up --remove-orphans -d", app);
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

ComposeAppEngine::AppState::AppState(const App& app, const boost::filesystem::path& root, bool set_version) try
    : version_file_ {
  (root / MetaDir / VersionFile).string()
}
, state_file_{(root / MetaDir / StateFile).string()} {
  version_ = version_file_.readStr();
  if (app.uri.empty() || version_ == app.uri) {
    state_ = static_cast<State>(state_file_.read());
    return;
  }

  state_ = AppState::State::kUnknown;
  if (set_version) {
    version_ = app.uri;
    version_file_.write(version_);
  }
}
catch (const std::exception& exc) {
  LOG_ERROR << "Failed to read version or state file: " << exc.what();
}

void ComposeAppEngine::AppState::setState(const State& state) {
  state_file_.write(static_cast<int>(state));
  state_ = state;
}

void ComposeAppEngine::AppState::File::write(int val) { write(&val, sizeof(val)); }

void ComposeAppEngine::AppState::File::write(const std::string& val) { write(val.data(), val.size()); }

int ComposeAppEngine::AppState::File::read() const {
  int val{0};
  read(&val, sizeof(val));
  return val;
}

std::string ComposeAppEngine::AppState::File::readStr() const {
  std::array<char, 4096> buf{0};
  read(&buf, buf.size());
  return buf.data();
}

int ComposeAppEngine::AppState::File::open(const char* file) {
  int fd = ::open(file, O_CREAT | O_RDWR | O_SYNC, S_IRUSR | S_IWUSR);
  if (-1 == fd) {
    throw std::system_error(errno, std::system_category(), std::string("Failed to open/create a file: ") + file);
  }
  return fd;
}

void ComposeAppEngine::AppState::File::write(const void* data, ssize_t size) {
  const std::string tmp_file{path_ + ".tmp"};
  int fd = open(tmp_file.c_str());
  int wr = ::write(fd, data, size);
  if (-1 == wr) {
    close(fd);
    throw std::system_error(errno, std::system_category(), std::string("Failed to write to a file: ") + path_);
  }
  fsync(fd);
  close(fd);

  int rr = ::rename(tmp_file.c_str(), path_.c_str());
  if (-1 == rr) {
    ::remove(tmp_file.c_str());
    throw std::system_error(errno, std::system_category(), std::string("Failed to rename the tmp file to: ") + path_);
  }
}

void ComposeAppEngine::AppState::File::read(void* data, ssize_t size) const {
  int fd = open(path_.c_str());
  int rr = ::read(fd, data, size);
  close(fd);
  if (-1 == rr) {
    throw std::system_error(errno, std::system_category(), std::string("Failed to read from a file: ") + path_);
  }
}

}  // namespace Docker
