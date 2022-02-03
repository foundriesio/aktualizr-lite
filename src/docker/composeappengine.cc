#include "composeappengine.h"
#include "composeinfo.h"
#include "dockerclient.h"

#include <sys/statvfs.h>
#include <boost/process.hpp>

#include "exec.h"

namespace Docker {

ComposeAppEngine::ComposeAppEngine(boost::filesystem::path root_dir, std::string compose_bin,
                                   Docker::DockerClient::Ptr docker_client, Docker::RegistryClient::Ptr registry_client)
    : root_{std::move(root_dir)},
      compose_{std::move(compose_bin)},
      docker_client_{std::move(docker_client)},
      registry_client_{std::move(registry_client)} {}

AppEngine::Result ComposeAppEngine::fetch(const App& app) {
  boost::filesystem::create_directories(appRoot(app) / MetaDir);

  Result result{false};

  try {
    AppState state(app, appRoot(app), true);

    download(app);
    state.setState(AppState::State::kDownloaded);

    exec(compose_ + "config", "compose App validation failed", boost::process::start_dir = appRoot(app));
    state.setState(AppState::State::kVerified);

    pullImages(app);
    state.setState(AppState::State::kPulled);

    result = true;
  } catch (const std::exception& exc) {
    result = {false, exc.what()};
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
        installApp(app);
        if (!areContainersCreated(app)) {
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
        start(app);
        if (!areContainersCreated(app)) {
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
    LOG_ERROR << "Cannot start App that hasn't been fetched: " << exc.what();
  }
  return result;
}

void ComposeAppEngine::remove(const App& app) {
  try {
    const auto root_dir{appRoot(app)};
    exec(compose_ + "down", "failed to bring App down", boost::process::start_dir = root_dir);
    boost::filesystem::remove_all(root_dir);
  } catch (const std::exception& exc) {
    LOG_ERROR << "docker-compose was unable to bring down: " << exc.what();
  }
}

bool ComposeAppEngine::isFetched(const App& app) const {
  if (!boost::filesystem::exists(appRoot(app))) {
    return false;
  }
  bool res{false};
  try {
    AppState state(app, appRoot(app));
    res = ((state() == AppState::State::kPulled) || (state() >= AppState::State::kInstalled));
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get/set App state: " << exc.what();
  }
  return res;
}

bool ComposeAppEngine::isRunning(const App& app) const {
  if (!boost::filesystem::exists(appRoot(app))) {
    return false;
  }

  bool started_state = false;
  try {
    AppState state(app, appRoot(app));
    if (app.uri == state.version()) {
      started_state = state() == AppState::State::kStarted;
    } else {
      // The state file exists but it describes a state of some other App not the one
      // that specified via the param. It can happen if a new Target App download or start fails
      // and aklite checks if the current App is running
      LOG_DEBUG << "Failed to get/set App state, fallback to checking the dockerd state";
      started_state = true;
    }
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
      const auto container_state{docker_client_->getContainerState(containers, app.name, service, hash)};
      if (std::get<0>(container_state) /* container exists */ &&
          std::get<1>(container_state) != "created" /* container was started */) {
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

      if (!apps.isMember(app_name) && AppState::exists(root_ / app_name)) {
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

std::pair<bool, std::string> ComposeAppEngine::cmd(const std::string& cmd) {
  std::string out_str;
  int exit_code = Utils::shell(cmd, &out_str, true);
  LOG_TRACE << "Command: " << cmd << "\n" << out_str;

  return {(exit_code == EXIT_SUCCESS), out_str};
}

void ComposeAppEngine::download(const App& app) {
  LOG_DEBUG << app.name << ": downloading App from Registry: " << app.uri;

  Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  Manifest manifest{registry_client_->getAppManifest(uri, Manifest::Format)};

  const std::string archive_file_name{uri.digest.shortHash() + '.' + app.name + ArchiveExt};
  Docker::Uri archive_uri{uri.createUri(Docker::HashedDigest(manifest.archiveDigest()))};

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
  verifyAppArchive(app, archive_file_name);
  extractAppArchive(app, archive_file_name);

  LOG_DEBUG << app.name << ": App has been downloaded";
}

AppEngine::Result ComposeAppEngine::verify(const App& app) {
  Result result{true};
  try {
    LOG_INFO << "Validating compose file";
    exec(compose_ + "config", "compose file validation failed", boost::process::start_dir = appRoot(app));
  } catch (const std::exception& exc) {
    result = {false, exc.what()};
  }
  return result;
}

void ComposeAppEngine::pullImages(const App& app) {
  LOG_INFO << "Pulling containers";
  exec(compose_ + "pull --no-parallel", "failed to pull App images", boost::process::start_dir = appRoot(app));
}

void ComposeAppEngine::installApp(const App& app) {
  LOG_INFO << "Installing App";
  exec(compose_ + "up --remove-orphans --no-start", "failed to install App", boost::process::start_dir = appRoot(app));
}

void ComposeAppEngine::start(const App& app) {
  LOG_INFO << "Starting App: " << app.name << " -> " << app.uri;
  exec(compose_ + "up --remove-orphans -d", "failed to start App", boost::process::start_dir = appRoot(app));
}

bool ComposeAppEngine::areContainersCreated(const App& app) {
  bool result{false};
  try {
    ComposeInfo info((appRoot(app) / ComposeFile).string());
    std::vector<Json::Value> services = info.getServices();
    if (services.empty()) {
      LOG_ERROR << "App: " << app.name << ", no services in docker file!";
      return false;
    }

    Json::Value containers;
    docker_client_->getContainers(containers);

    for (std::size_t i = 0; i < services.size(); i++) {
      std::string service = services[i].asString();
      std::string hash = info.getHash(services[i]);
      const auto container_state{docker_client_->getContainerState(containers, app.name, service, hash)};
      if (std::get<0>(container_state) /* container exists */) {
        continue;
      }
      LOG_WARNING << "App: " << app.name << ", service: " << service << ", hash: " << hash << ", not running!";
      return false;
    }

    result = true;
  } catch (...) {
    LOG_WARNING << "App: " << app.name << ", cant check if it is running!";
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

void ComposeAppEngine::verifyAppArchive(const App& app, const std::string& archive_file_name) {
  try {
    exec("tar -tf " + archive_file_name + " " + ComposeFile, "no compose file found in archive",
         boost::process::start_dir = appRoot(app));
  } catch (const std::exception&) {
    exec("tar -tf " + archive_file_name + " ./" + ComposeFile, "no compose file found in archive",
         boost::process::start_dir = appRoot(app));
  }
}

void ComposeAppEngine::extractAppArchive(const App& app, const std::string& archive_file_name,
                                         bool delete_after_extraction) {
  exec("tar -xzf " + archive_file_name, "failed to extract App archive", boost::process::start_dir = appRoot(app));
  if (delete_after_extraction) {
    exec("rm -f " + archive_file_name, "failed to delete App archive", boost::process::start_dir = appRoot(app));
  }
}

void ComposeAppEngine::pruneDockerStore() {
  LOG_INFO << "Pruning unused docker images";
  // Utils::shell which isn't interactive, we'll use std::system so that
  // stdout/stderr is streamed while docker sets things up.
  if (std::system("docker image prune -a -f --filter=\"label!=aktualizr-no-prune\"") != 0) {
    LOG_WARNING << "Unable to prune unused docker images";
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

bool ComposeAppEngine::AppState::exists(const boost::filesystem::path& root) {
  return boost::filesystem::exists(root / MetaDir / VersionFile) &&
         boost::filesystem::exists(root / MetaDir / StateFile);
}

void ComposeAppEngine::AppState::setState(const State& state) {
  try {
    state_file_.write(static_cast<int>(state));
    state_ = state;
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to set App state: " << exc.what();
  }
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
