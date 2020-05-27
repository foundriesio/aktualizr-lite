#include "composeappmanager.h"

#include <sys/statvfs.h>

#include "boost/process.hpp"
#include "crypto/crypto.h"
#include "http/httpclient.h"
#include "json/json.h"

struct ComposeApp {
  ComposeApp(std::string name, const ComposeAppConfig& config, const Docker::RegistryClient& registry_client)
      : name_{std::move(name)},
        root_{config.apps_root / name_},
        compose_{boost::filesystem::canonical(config.compose_bin).string() + " "},
        registry_client_{registry_client} {}

  static constexpr const char* const ArchiveExt{".tgz"};

  struct Manifest : Json::Value {
    static constexpr const char* const Format{"application/vnd.oci.image.manifest.v1+json"};
    static constexpr const char* const Version{"v1"};

    Manifest(const Json::Value& value = Json::Value()) : Json::Value(value) {
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
  bool cmd_streaming(const std::string& cmd) {
    LOG_DEBUG << "Running: " << cmd;
    return boost::process::system(cmd, boost::process::start_dir = root_) == 0;
  }

  bool fetch(const std::string& app_uri) {
    boost::filesystem::create_directories(root_);
    //    if (cmd_streaming(compose_ + "download " + app_uri)) {
    if (download(app_uri)) {
      LOG_INFO << "Validating compose file";
      if (cmd_streaming(compose_ + "config")) {
        LOG_INFO << "Pulling containers";
        return cmd_streaming(compose_ + "pull");
      }
    }
    return false;
  };

  bool start() { return cmd_streaming(compose_ + "up --remove-orphans -d"); }

  void remove() {
    if (cmd_streaming(compose_ + "down")) {
      boost::filesystem::remove_all(root_);
    } else {
      LOG_ERROR << "docker-compose was unable to bring down: " << root_;
    }
  }

 private:
  bool download(const std::string& app_uri);
  static bool checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size);
  void extractAppArchive(const std::string& archive_file_name, bool delete_after_extraction = true);

 private:
  std::string name_;
  boost::filesystem::path root_;
  std::string compose_;
  const Docker::RegistryClient& registry_client_;
};

ComposeAppConfig::ComposeAppConfig(const PackageConfig& pconfig) {
  const std::map<std::string, std::string> raw = pconfig.extra;

  if (raw.count("compose_apps") == 1) {
    std::string val = raw.at("compose_apps");
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(apps, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }
  if (raw.count("compose_apps_root") == 1) {
    apps_root = raw.at("compose_apps_root");
  }
  if (raw.count("docker_compose_bin") == 1) {
    compose_bin = raw.at("docker_compose_bin");
  }

  if (raw.count("docker_prune") == 1) {
    std::string val = raw.at("docker_prune");
    boost::algorithm::to_lower(val);
    docker_prune = val != "0" && val != "false";
  }
  // if not specified, let's extract it from the `ostree_server` value if it's defined
  if (!pconfig.ostree_server.empty()) {
    const std::string treehub_endpoint = "treehub";
    const std::string registry_creds_endpoint = "hub-creds/";
    registry_conf.auth_creds_endpoint = pconfig.ostree_server;
    auto endpoint_pos = registry_conf.auth_creds_endpoint.find(treehub_endpoint);
    if (endpoint_pos != std::string::npos) {
      registry_conf.auth_creds_endpoint.replace(endpoint_pos, registry_creds_endpoint.length(),
                                                registry_creds_endpoint);
    }
  }
}

ComposeAppManager::ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                                     const std::shared_ptr<INvStorage>& storage,
                                     const std::shared_ptr<HttpInterface>& http,
                                     Docker::RegistryClient::HttpClientFactory registry_http_client_factory)
    : OstreeManager(pconfig, bconfig, storage, http),
      cfg_{pconfig},
      registry_client_{cfg_.registry_conf, http, std::move(registry_http_client_factory)} {}

std::vector<std::pair<std::string, std::string>> ComposeAppManager::getApps(const Uptane::Target& t) const {
  std::vector<std::pair<std::string, std::string>> apps;

  auto target_apps = t.custom_data()["docker_compose_apps"];
  for (Json::ValueIterator i = target_apps.begin(); i != target_apps.end(); ++i) {
    if ((*i).isObject() && (*i).isMember("uri")) {
      for (const auto& app : cfg_.apps) {
        if (i.key().asString() == app) {
          apps.emplace_back(app, (*i)["uri"].asString());
          break;
        }
      }
    } else {
      LOG_ERROR << "Invalid custom data for docker_compose_app: " << i.key().asString() << " -> " << *i;
    }
  }

  return apps;
}

bool ComposeAppManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    FetcherProgressCb progress_cb, const api::FlowControlToken* token) {
  if (!OstreeManager::fetchTarget(target, fetcher, keys, progress_cb, token)) {
    return false;
  }

  LOG_INFO << "Looking for Compose Apps to fetch";
  bool passed = true;
  for (const auto& pair : getApps(target)) {
    LOG_INFO << "Fetching " << pair.first << " -> " << pair.second;
    if (!ComposeApp(pair.first, cfg_, registry_client_).fetch(pair.second)) {
      passed = false;
    }
  }
  return passed;
}

data::InstallationResult ComposeAppManager::install(const Uptane::Target& target) const {
  data::InstallationResult res;
  Uptane::Target current = OstreeManager::getCurrent();
  if (current.sha256Hash() != target.sha256Hash()) {
    // notify the bootloader before installation happens as it is not atomic
    // and a false notification doesn't hurt with rollback support in place
    updateNotify();
    res = OstreeManager::install(target);
    if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
      LOG_ERROR << "Failed to install OSTree target, skipping Docker Compose Apps";
      return res;
    }
  } else {
    LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
    res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
  }

  handleRemovedApps(target);
  for (const auto& pair : getApps(target)) {
    LOG_INFO << "Installing " << pair.first << " -> " << pair.second;
    if (!ComposeApp(pair.first, cfg_, registry_client_).start()) {
      res = data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, "Could not install app");
    }
  };

  if (cfg_.docker_prune) {
    LOG_INFO << "Pruning unused docker images";
    // Utils::shell which isn't interactive, we'll use std::system so that
    // stdout/stderr is streamed while docker sets things up.
    if (std::system("docker image prune -a -f --filter=\"label!=aktualizr-no-prune\"") != 0) {
      LOG_WARNING << "Unable to prune unused docker images";
    }
  }

  return res;
}

// Handle the case like:
//  1) sota.toml is configured with 2 compose apps: "app1, app2"
//  2) update is applied, so we are now running both app1 and app2
//  3) sota.toml is updated with 1 docker app: "app1"
// At this point we should stop app2 and remove it.
void ComposeAppManager::handleRemovedApps(const Uptane::Target& target) const {
  if (!boost::filesystem::is_directory(cfg_.apps_root)) {
    LOG_DEBUG << "cfg_.apps_root does not exist";
    return;
  }
  std::vector<std::string> target_apps = target.custom_data()["docker_compose_apps"].getMemberNames();

  for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(cfg_.apps_root), {})) {
    if (boost::filesystem::is_directory(entry)) {
      std::string name = entry.path().filename().native();
      if (std::find(cfg_.apps.begin(), cfg_.apps.end(), name) == cfg_.apps.end()) {
        LOG_WARNING << "Docker Compose App(" << name
                    << ") installed, but is now removed from configuration. Removing from system";
        ComposeApp(name, cfg_, registry_client_).remove();
      } else if (std::find(target_apps.begin(), target_apps.end(), name) == target_apps.end()) {
        LOG_WARNING << "Docker Compose App(" << name
                    << ") configured, but not defined in installation target. Removing from system";
        ComposeApp(name, cfg_, registry_client_).remove();
      }
    }
  }
}

bool ComposeApp::checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size) {
  struct statvfs stat_buf {};
  const int stat_res = statvfs(app_root.c_str(), &stat_buf);
  if (stat_res < 0) {
    LOG_WARNING << "Unable to read filesystem statistics: error code " << stat_res;
    return false;
  }
  const uint64_t available_bytes = (static_cast<uint64_t>(stat_buf.f_bsize) * stat_buf.f_bavail);
  const uint64_t reserved_bytes = 1 << 20;

  out_available_size = (available_bytes - reserved_bytes);
  return true;
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
      // 80% is a storage space watermark, we don't want to fill a volume above it
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

    result = true;
    LOG_DEBUG << name_ << ": App has been downloaded";
  } catch (const std::exception& exc) {
    LOG_ERROR << name_ << ": failed to download App from Registry: " << exc.what();
  }

  return result;
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

namespace Docker {

Uri Uri::parseUri(const std::string& uri) {
  auto split_pos = uri.find('@');
  if (split_pos == std::string::npos) {
    throw std::invalid_argument("Invalid App URI: '@' not found in " + uri);
  }

  auto app_name_pos = uri.rfind('/', split_pos);
  if (app_name_pos == std::string::npos) {
    throw std::invalid_argument("Invalid App URI: the app name not found in " + uri);
  }

  auto app = uri.substr(app_name_pos + 1, split_pos - app_name_pos - 1);
  auto digest = uri.substr(split_pos + 1);
  LOG_DEBUG << app << ": App digest: " << digest;

  auto factory_name_pos = uri.rfind('/', app_name_pos - 1);
  if (factory_name_pos == std::string::npos) {
    throw std::invalid_argument("Invalid App URI; the app factory name not found in " + uri);
  }

  auto factory = uri.substr(factory_name_pos + 1, app_name_pos - factory_name_pos - 1);
  LOG_DEBUG << app << ": Factory: " << factory;

  auto repo = uri.substr(factory_name_pos + 1, split_pos - factory_name_pos - 1);
  LOG_DEBUG << app << ": App Repo: " << repo;

  auto registry_hostname = uri.substr(0, factory_name_pos);
  LOG_DEBUG << app << ": App Registry hostname: " << registry_hostname;

  return Uri{HashedDigest{digest}, app, factory, repo, registry_hostname};
}

Uri Uri::createUri(const HashedDigest& digest_in) {
  return Uri{HashedDigest{digest_in}, app, factory, repo, registryHostname};
}

RegistryClient::HttpClientFactory RegistryClient::DefaultHttpClientFactory =
    [](const std::vector<std::string>* headers) { return std::make_shared<HttpClient>(headers); };

const std::string HashedDigest::Type{"sha256:"};

HashedDigest::HashedDigest(const std::string& hash_digest) : digest_{boost::algorithm::to_lower_copy(hash_digest)} {
  if (Type != digest_.substr(0, Type.length())) {
    throw std::invalid_argument("Unsupported hash type: " + hash_digest);
  }

  hash_ = digest_.substr(Type.length());
  if (64 != hash_.size()) {
    throw std::invalid_argument("Invalid hash size: " + hash_digest);
  }

  short_hash_ = hash_.substr(0, 7);
}

const std::string RegistryClient::ManifestEndpoint{"/manifests/"};
const std::string RegistryClient::BlobEndpoint{"/blobs/"};
const std::string RegistryClient::SupportedRegistryVersion{"/v2/"};

Json::Value RegistryClient::getAppManifest(const Uri& uri, const std::string& format) const {
  const std::string manifest_url{composeManifestUrl(uri)};
  LOG_DEBUG << "Downloading App manifest: " << manifest_url;

  std::vector<std::string> registry_repo_request_headers{getBearerAuthHeader(uri), "accept:" + format};
  auto registry_repo_client{http_client_factory_(&registry_repo_request_headers)};

  auto manifest_resp = registry_repo_client->get(manifest_url, ManifestMaxSize);
  if (!manifest_resp.isOk()) {
    throw std::runtime_error("Failed to download App manifest: " + manifest_resp.getStatusStr());
  }

  if (manifest_resp.body.size() > ManifestMaxSize) {
    throw std::runtime_error("Size of received App manifest exceeds the maximum allowed: " +
                             std::to_string(manifest_resp.body.size()) + " > " + std::to_string(ManifestMaxSize));
  }

  auto received_manifest_hash{
      boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(manifest_resp.body)))};

  if (received_manifest_hash != uri.digest.hash()) {
    throw std::runtime_error(
        "Hash of received App manifest and the hash specified in Target"
        " do not match: " +
        received_manifest_hash + " != " + uri.digest.hash());
  }

  LOG_TRACE << "Received App manifest: \n" << manifest_resp.getJson();
  return ComposeApp::Manifest{manifest_resp.getJson()};
}

struct DownloadCtx {
  DownloadCtx(std::ostream& out_stream_in, MultiPartHasher& hasher_in, std::size_t expected_size_in)
      : out_stream{out_stream_in}, hasher{hasher_in}, expected_size{expected_size_in} {}

  std::ostream& out_stream;
  MultiPartHasher& hasher;
  std::size_t expected_size;

  std::size_t written_size{0};
  std::size_t received_size{0};

  std::size_t write(const char* data, std::size_t size) {
    assert(data);

    received_size = written_size + size;
    if (received_size > expected_size) {
      LOG_ERROR << "!!! Received data size exceeds the expected size: " << received_size << " != " << expected_size;
      return (size + 1);  // returning value that is not equal to received data size will make curl fail
    }

    if (!out_stream.good()) {
      LOG_ERROR << "Output stream is at a bad state: " << out_stream.rdstate();
      return (size + 1);  // returning value that is not equal to received data size will make curl fail
    }

    auto start_pos = out_stream.tellp();
    out_stream.write(data, size);
    auto end_pos = out_stream.tellp();

    written_size += (end_pos - start_pos);
    hasher.update(reinterpret_cast<const unsigned char*>(data), size);
    return (end_pos - start_pos);
  }
};

static size_t DownloadHandler(char* data, size_t buf_size, size_t buf_numb, void* user_ctx) {
  assert(user_ctx);

  auto download_ctx = reinterpret_cast<DownloadCtx*>(user_ctx);
  return download_ctx->write(data, (buf_size * buf_numb));
}

void RegistryClient::downloadBlob(const Uri& uri, const boost::filesystem::path& filepath, size_t expected_size) const {
  auto compose_app_blob_url{composeBlobUrl(uri)};

  LOG_DEBUG << "Downloading App blob: " << compose_app_blob_url;

  std::vector<std::string> registry_repo_request_headers{getBearerAuthHeader(uri)};
  auto registry_repo_client{http_client_factory_(&registry_repo_request_headers)};

  std::ofstream output_file{filepath.string(), std::ios_base::out | std::ios_base::binary};
  if (!output_file.is_open()) {
    throw std::runtime_error("Failed to open a file: " + filepath.string());
  }
  MultiPartSHA256Hasher hasher;

  DownloadCtx download_ctx{output_file, hasher, expected_size};
  auto get_blob_resp = registry_repo_client->download(compose_app_blob_url, DownloadHandler, nullptr, &download_ctx, 0);

  if (!get_blob_resp.isOk()) {
    throw std::runtime_error("Failed to download App blob: " + get_blob_resp.getStatusStr());
  }

  output_file.close();
  std::size_t recv_blob_file_size{download_ctx.written_size};

  if (recv_blob_file_size != expected_size) {
    std::remove(filepath.c_str());
    throw std::runtime_error(
        "Size of downloaded App blob does not equal to "
        "the expected one: " +
        std::to_string(recv_blob_file_size) + " != " + std::to_string(expected_size));
  }

  auto recv_blob_hash{boost::algorithm::to_lower_copy(hasher.getHexDigest())};

  if (recv_blob_hash != uri.digest.hash()) {
    std::remove(filepath.c_str());
    throw std::runtime_error(
        "Hash of downloaded App blob does not equal to "
        "the expected one: " +
        recv_blob_hash + " != " + uri.digest.hash());
  }
}

std::string RegistryClient::getBasicAuthHeader() const {
  // TODO: to make it working against any Registry, not just FIO's one
  // we will need to make use of the Docker's mechanisms for it,
  // specifically in docker/config.json there should defined an auth material and/or credHelpers
  // for a given registry. If auth material is defined then just use it if not then try to invoke
  // a script/executbale defined in credHelpers  that is supposed to return an auth material
  LOG_DEBUG << "Getting Docker Registry credentials from " << conf_.auth_creds_endpoint;

  auto creds_resp = ota_lite_client_->get(conf_.auth_creds_endpoint, AuthMaterialMaxSize);

  if (!creds_resp.isOk()) {
    throw std::runtime_error("Failed to get Docker Registry credentials from " + conf_.auth_creds_endpoint +
                             "; error: " + creds_resp.getStatusStr());
  }

  auto creds_json = creds_resp.getJson();
  auto username = creds_json["Username"].asString();
  auto secret = creds_json["Secret"].asString();

  if (username.empty() || secret.empty()) {
    throw std::runtime_error("Got invalid Docker Registry credentials: " + creds_resp.body);
  }

  std::string auth_secret_str{username + ':' + secret};
  auto encoded_auth_secret = Utils::toBase64(auth_secret_str);

  LOG_DEBUG << "Got Docker Registry credentials, username: " << username;
  return "authorization: basic " + encoded_auth_secret;
}

std::string RegistryClient::getBearerAuthHeader(const Uri& uri) const {
  // TODO: to make it generic we need to make a request for a resource at first
  // and then if we get 401 we should parse 'Www-Authenticate' header and get
  // URL and params of the request for a token from it.
  // Currently, we support just FIO's Registry so we know its endpoint and what params we need to send
  // so we do shortcut here.
  // The aktualizr HTTP client doesn't return headers in the response object so adding this functionality
  // implies making changes in libaktualizr or writing our own http client what is not justifiable at the moment
  std::string auth_token_endpoint{"https://" + uri.registryHostname + "/token-auth/"};
  LOG_DEBUG << "Getting Docker Registry token from " << auth_token_endpoint;

  std::vector<std::string> auth_header = {getBasicAuthHeader()};

  auto registry_client{http_client_factory_(&auth_header)};
  std::string token_req_params{"?service=registry&scope=repository:" + uri.repo + ":pull"};

  auto token_resp = registry_client->get(auth_token_endpoint + token_req_params, AuthMaterialMaxSize);

  if (!token_resp.isOk()) {
    throw std::runtime_error("Failed to get Auth Token at Docker Registry " + auth_token_endpoint +
                             "; error: " + token_resp.getStatusStr());
  }

  auto token = token_resp.getJson()["token"].asString();
  if (token.empty()) {
    throw std::runtime_error("Got invalid token from Docker Registry: " + token_resp.body);
  }

  LOG_DEBUG << "Got Docker Registry token: " << token;
  return "authorization: bearer " + token;
}

}  // namespace Docker
