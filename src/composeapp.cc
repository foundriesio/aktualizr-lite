#include "composeapp.h"

#include <sys/statvfs.h>
#include <boost/process.hpp>

namespace Docker {

ComposeApp::ComposeApp(std::string name, const boost::filesystem::path& root_dir, std::string compose_bin,
                       std::string docker_bin, const Docker::RegistryClient& registry_client)
    : name_{std::move(name)},
      root_{root_dir / name_},
      compose_{std::move(compose_bin)},
      docker_{std::move(docker_bin)},
      registry_client_{registry_client} {}

bool ComposeApp::fetch(const std::string& app_uri) {
  boost::filesystem::create_directories(root_);
  if (download(app_uri)) {
    LOG_INFO << "Validating compose file";
    if (cmd_streaming(compose_ + "config")) {
      LOG_INFO << "Pulling containers";
      return cmd_streaming(compose_ + "pull --no-parallel");
    }
  }
  return false;
};

bool ComposeApp::up(bool no_start) {
  const auto* mode = no_start ? "--no-start" : "-d";

  if (no_start) {
    boost::filesystem::ofstream flag_file(root_ / NeedStartFile);
  }

  return cmd_streaming(compose_ + "up --remove-orphans " + mode);
}

bool ComposeApp::start() { return cmd_streaming(compose_ + "start"); }

void ComposeApp::remove() {
  if (cmd_streaming(compose_ + "down")) {
    boost::filesystem::remove_all(root_);
  } else {
    LOG_ERROR << "docker-compose was unable to bring down: " << root_;
  }
}

bool ComposeApp::isRunning() {
  bool cmd_res{false};
  std::string cmd_output;
  std::tie(cmd_res, cmd_output) = cmd("cat " + (root_ / ComposeFile).string());
  if (!cmd_res) {
    LOG_WARNING << "Failed to parse App config: " << name_;
    return false;
  }

  // calculate a number of container images that a given App consists of
  std::string::size_type find_pos{0};
  std::string::size_type line_pos{0};
  int expected_container_number{0};

  while ((find_pos = cmd_output.find("image:", line_pos)) != std::string::npos) {
    if (cmd_output.substr(line_pos, (find_pos - line_pos)).find_first_of('#') == std::string::npos) {
      ++expected_container_number;
    }
    line_pos = cmd_output.find('\n', find_pos);
    ++find_pos;
  }

  // Get a number of running container images
  std::tie(cmd_res, cmd_output) =
      cmd(docker_ + "ps -q --filter=status=running --filter=label=com.docker.compose.project=" + name_);
  if (!cmd_res) {
    LOG_WARNING << "Failed to get a list of App's containers: " << name_;
    return false;
  }

  int running_container_number =
      std::count_if(cmd_output.begin(), cmd_output.end(), [](const char& symbol) { return symbol == '\n'; });

  if (running_container_number < expected_container_number) {
    LOG_DEBUG << "Number of running containers is less than a number of images specified in the compose file"
              << "; App: " << name_ << "; expected container number: " << expected_container_number
              << "; number of running containers: " << running_container_number;
    return false;
  }

  return true;
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
bool ComposeApp::cmd_streaming(const std::string& cmd) {
  LOG_DEBUG << "Running: " << cmd;
  int exit_code = boost::process::system(cmd, boost::process::start_dir = root_);
  return exit_code == 0;
}

std::pair<bool, std::string> ComposeApp::cmd(const std::string& cmd) {
  std::string out_str;
  int exit_code = Utils::shell(cmd, &out_str, true);
  LOG_TRACE << "Command: " << cmd << "\n" << out_str;

  return {(exit_code == EXIT_SUCCESS), out_str};
}

bool ComposeApp::download(const std::string& app_uri) {
  bool result = false;

  try {
    LOG_DEBUG << name_ << ": downloading App from Registry: " << app_uri;

    Docker::Uri uri{Docker::Uri::parseUri(app_uri)};
    Manifest manifest{registry_client_.getAppManifest(uri, Manifest::Format)};

    const std::string archive_file_name{uri.digest.shortHash() + '.' + name_ + ArchiveExt};
    Docker::Uri archive_uri{uri.createUri(manifest.archiveDigest())};

    uint64_t available_storage;
    if (checkAvailableStorageSpace(root_, available_storage)) {
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

    registry_client_.downloadBlob(archive_uri, root_ / archive_file_name, manifest.archiveSize());
    extractAppArchive(archive_file_name);
    Utils::writeFile(root_ / ".app_uri", app_uri);

    result = true;
    LOG_DEBUG << name_ << ": App has been downloaded";
  } catch (const std::exception& exc) {
    LOG_ERROR << name_ << ": failed to download App from Registry: " << exc.what();
  }

  return result;
}

bool ComposeApp::checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size) {
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

void ComposeApp::extractAppArchive(const std::string& archive_file_name, bool delete_after_extraction) {
  if (!cmd_streaming("tar -xzf " + archive_file_name)) {
    throw std::runtime_error("Failed to extract the compose app archive: " + archive_file_name);
  }
  if (delete_after_extraction) {
    if (!cmd_streaming("rm -f " + archive_file_name)) {
      throw std::runtime_error("Failed to remove the compose app archive: " + archive_file_name);
    }
  }
}

}  // namespace Docker
