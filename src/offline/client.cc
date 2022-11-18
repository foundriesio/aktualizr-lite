#include "client.h"

#include <boost/process.hpp>

#include "aktualizr-lite/api.h"
#include "appengine.h"
#include "composeappmanager.h"
#include "docker/composeinfo.h"
#include "docker/docker.h"
#include "docker/restorableappengine.h"
#include "ostree/repo.h"
#include "storage/invstorage.h"
#include "target.h"

namespace offline {
namespace client {

static std::unique_ptr<LiteClient> createOfflineClient(
    const Config& cfg_in, const UpdateSrc& src,
    std::shared_ptr<HttpInterface> docker_client_http_client =
        Docker::DockerClient::DefaultHttpClientFactory("unix:///var/run/docker.sock"));
static Uptane::Target getTarget(LiteClient& client, const std::string& target_name = "");
static void registerApps(const Uptane::Target& target);

class BaseHttpClient : public HttpInterface {
 public:
  HttpResponse post(const std::string&, const std::string&, const std::string&) override {
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse post(const std::string&, const Json::Value&) override { return HttpResponse("", 501, CURLE_OK, ""); }
  HttpResponse put(const std::string&, const std::string&, const std::string&) override {
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse put(const std::string&, const Json::Value&) override { return HttpResponse("", 501, CURLE_OK, ""); }
  HttpResponse download(const std::string& url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void* userp, curl_off_t from) override {
    return HttpResponse("", 501, CURLE_OK, "");
  }
  std::future<HttpResponse> downloadAsync(const std::string& url, curl_write_callback write_cb,
                                          curl_xferinfo_callback progress_cb, void* userp, curl_off_t from,
                                          CurlHandler* easyp) override {
    std::promise<HttpResponse> resp_promise;
    resp_promise.set_value(HttpResponse("", 501, CURLE_OK, ""));
    return resp_promise.get_future();
  }
  void setCerts(const std::string&, CryptoSource, const std::string&, CryptoSource, const std::string&,
                CryptoSource) override {}
};

class RegistryBasicAuthClient : public BaseHttpClient {
 public:
  HttpResponse get(const std::string& url, int64_t maxsize) override {
    return HttpResponse("{\"Secret\":\"secret\",\"Username\":\"test-user\"}", 200, CURLE_OK, "");
  }
};

class OfflineRegistry : public BaseHttpClient {
 public:
  OfflineRegistry(const boost::filesystem::path& root_dir, const std::string& hostname = "hub.foundries.io")
      : hostname_{hostname}, root_dir_{root_dir} {}

  HttpResponse get(const std::string& url, int64_t maxsize) override {
    if (boost::starts_with(url, auth_endpoint_)) {
      return HttpResponse("{\"token\":\"token\"}", 200, CURLE_OK, "");
    }
    return getAppItem(url);
  }

  HttpResponse download(const std::string& url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void* userp, curl_off_t from) override {
    const std::string hash_prefix{"sha256:"};
    const auto digest_pos{url.rfind(hash_prefix)};
    if (digest_pos == std::string::npos) {
      return HttpResponse("Invalid URL", 400, CURLE_OK, "");
    }
    const auto hash_pos{digest_pos + hash_prefix.size()};
    const auto hash{url.substr(hash_pos)};

    char buf[1024 * 4];
    std::ifstream blob_file{(blobs_dir_ / hash).string()};

    std::streamsize read_byte_numb;
    while ((read_byte_numb = blob_file.readsome(buf, sizeof(buf))) > 0) {
      write_cb(buf, read_byte_numb, 1, userp);
    }
    blob_file.close();
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse getAppItem(const std::string& url) const {
    const std::string hash_prefix{"sha256:"};
    const auto digest_pos{url.rfind(hash_prefix)};
    if (digest_pos == std::string::npos) {
      return HttpResponse("Invalid URL", 400, CURLE_OK, "");
    }
    const auto hash_pos{digest_pos + hash_prefix.size()};
    const auto hash{url.substr(hash_pos)};
    return HttpResponse(Utils::readFile(blobs_dir_ / hash), 200, CURLE_OK, "");
  }

  boost::filesystem::path blobsDir() const { return root_dir_ / "blobs"; }
  const boost::filesystem::path& appsDir() const { return apps_dir_; }
  const boost::filesystem::path& dir() const { return root_dir_; }

 private:
  const boost::filesystem::path root_dir_;
  const std::string hostname_;
  const std::string auth_endpoint_{"https://" + hostname_ + "/token-auth"};
  const boost::filesystem::path apps_dir_{root_dir_ / "apps"};
  const boost::filesystem::path blobs_dir_{root_dir_ / "blobs" / "sha256"};
};

static std::unique_ptr<LiteClient> createOfflineClient(const Config& cfg_in, const UpdateSrc& src,
                                                       std::shared_ptr<HttpInterface> docker_client_http_client) {
  Config cfg{cfg_in};  // make copy of the input config to avoid its modification by LiteClient

  // turn off reporting update events to DG
  cfg.tls.server = "";
  // make LiteClient to pull from a local ostree repo
  cfg.pacman.ostree_server = "file://" + src.OstreeRepoDir.string();

  // Always use the compose app manager since it covers both use-cases, just ostree and ostree+apps.
  cfg.pacman.type = ComposeAppManager::Name;
  // Unless there is no `docker` or `dockerd`
  if (!boost::filesystem::exists("/usr/bin/dockerd") || !boost::filesystem::exists("/usr/bin/docker")) {
    cfg.pacman.type = RootfsTreeManager::Name;
    return std::make_unique<LiteClient>(cfg, nullptr, nullptr, std::make_shared<MetaFetcher>(src.TufDir));
  }

  // Handle DG:/token-auth
  std::shared_ptr<HttpInterface> registry_basic_auth_client{std::make_shared<RegistryBasicAuthClient>()};

  std::shared_ptr<OfflineRegistry> offline_registry{std::make_shared<OfflineRegistry>(src.AppsDir)};
  // Handle requests to Registry aimed to download App
  Docker::RegistryClient::Ptr registry_client{std::make_shared<Docker::RegistryClient>(
      registry_basic_auth_client, "",
      [offline_registry](const std::vector<std::string>*, const std::set<std::string>*) { return offline_registry; })};

  ComposeAppManager::Config pacman_cfg(cfg.pacman);
  std::string compose_cmd{pacman_cfg.compose_bin.string()};
  if (boost::filesystem::exists(pacman_cfg.compose_bin) && pacman_cfg.compose_bin.filename().compare("docker") == 0) {
    compose_cmd = boost::filesystem::canonical(pacman_cfg.compose_bin).string() + " ";
    // if it is a `docker` binary then turn it into ` the  `docker compose` command
    // and make sure that the `compose` is actually supported by a given `docker` utility.
    compose_cmd += "compose ";
  }

  std::string docker_host{"unix:///var/run/docker.sock"};
  auto env{boost::this_process::environment()};
  if (env.end() != env.find("DOCKER_HOST")) {
    docker_host = env.get("DOCKER_HOST");
  }

  AppEngine::Ptr app_engine{std::make_shared<Docker::RestorableAppEngine>(
      pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, registry_client,
      std::make_shared<Docker::DockerClient>(docker_client_http_client), pacman_cfg.skopeo_bin.string(), docker_host,
      compose_cmd, Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
      [offline_registry](const Docker::Uri& app_uri, const std::string& image_uri) {
        Docker::Uri uri{Docker::Uri::parseUri(image_uri, false)};
        return "--src-shared-blob-dir " + offline_registry->blobsDir().string() +
               " oci:" + offline_registry->appsDir().string() + "/" + app_uri.app + "/" + app_uri.digest.hash() +
               "/images/" + uri.registryHostname + "/" + uri.repo + "/" + uri.digest.hash();
      },
      false /* don't create containers on install because it makes dockerd check if pinned images
    present in its store what we should avoid until images are registered (hacked) in dockerd store
  */)};

  return std::make_unique<LiteClient>(cfg, app_engine, nullptr, std::make_shared<MetaFetcher>(src.TufDir));
}

static Uptane::Target getSpecificTarget(LiteClient& client, const std::string& target_name) {
  const auto found_target_it =
      std::find_if(client.allTargets().begin(), client.allTargets().end(),
                   [&target_name](const Uptane::Target& target) { return target_name == target.filename(); });
  if (found_target_it != client.allTargets().end()) {
    return *found_target_it;
  } else {
    return Uptane::Target::Unknown();
  }
}

static void parseUpdateContent(const Uptane::HardwareIdentifier& hw_id,
                               const boost::filesystem::path& src_ostree_repo_dir,
                               const boost::filesystem::path& apps_dir, std::vector<std::string>& found_ostree_commits,
                               std::vector<std::string>& found_apps) {
  {  // parse ostree repo
    OSTree::Repo ostree_repo{src_ostree_repo_dir.string()};
    LOG_INFO << "Parsing a source ostree repo: " << src_ostree_repo_dir.string();
    std::unordered_map<std::string, std::string> refs_map{ostree_repo.getRefs()};
    for (const auto& ref : refs_map) {
      found_ostree_commits.push_back(ref.second);
    }
  }
  {
    if (!boost::filesystem::exists(apps_dir)) {
      return;
    }
    for (auto const& app_dir_entry : boost::filesystem::directory_iterator{apps_dir}) {
      const auto app_name{app_dir_entry.path().filename().string()};
      for (auto const& app_ver_dir_entry : boost::filesystem::directory_iterator{app_dir_entry.path()}) {
        const auto uri_file{app_ver_dir_entry.path() / "uri"};
        const auto app_uri{Utils::readFile(uri_file.string())};
        LOG_INFO << "Found app; uri: " << app_uri;
        found_apps.push_back(app_uri);
      }
    }
  }
}

static Uptane::Target getTarget(LiteClient& client, const UpdateSrc& src) {
  if (!src.TargetName.empty()) {
    return getSpecificTarget(client, src.TargetName);
  }

  // sort Targets by a version number in a descendent order
  auto target_sort = [](const Uptane::Target& t1, const Uptane::Target& t2) {
    return strverscmp(t1.custom_version().c_str(), t2.custom_version().c_str()) > 0;
  };
  std::set<Uptane::Target, decltype(target_sort)> available_targets(target_sort);

  for (const auto& target : client.allTargets()) {
    if (target.hardwareIds().size() != 1) {
      LOG_ERROR << "Invalid hardware ID number found in Target; target: " << target.filename()
                << "; found: " << target.hardwareIds().size() << "; expected: " << 1;
      continue;
    }
    const auto hwid{target.hardwareIds()[0]};
    if (client.primary_ecu.second != hwid) {
      LOG_DEBUG << "Found Target's hardware ID doesn't match a device's hardware ID, skipping it; target hw ID: "
                << hwid << "; device hw ID: " << client.primary_ecu.second;
      continue;
    }
    LOG_DEBUG << "Found Target: " << target.filename();
    available_targets.insert(target);
  }

  // parse the update content
  std::vector<std::string> found_ostree_commits;
  std::vector<std::string> found_apps;
  parseUpdateContent(client.primary_ecu.second, src.OstreeRepoDir, src.AppsDir / "apps", found_ostree_commits,
                     found_apps);

  // find Target that matches the given update content, search starting from the most recent Target
  Uptane::Target found_target(Uptane::Target::Unknown());
  for (const auto& t : available_targets) {
    LOG_INFO << "Checking if update content matches the given target: " << t.filename();
    if (found_ostree_commits.end() ==
        std::find(found_ostree_commits.begin(), found_ostree_commits.end(), t.sha256Hash())) {
      LOG_DEBUG << "No ostree commit found for Target: " << t.filename();
      continue;
    }

    std::list<std::string> found_but_not_target_apps{found_apps.begin(), found_apps.end()};
    auto shortlisted_target_apps{Target::appsJson(t)};

    for (const auto& app : Target::Apps(t)) {
      if (found_apps.end() == std::find(found_apps.begin(), found_apps.end(), app.uri)) {
        // It may happen because App was shortlisted during running the CI run that fetched Apps, so we `continue` with
        // the App matching We just need to make sure that all found/update Apps macthes subset of Target Apps
        LOG_DEBUG << "No App found for Target; Target: " << t.filename() << "; app: " << app.uri;
        shortlisted_target_apps.removeMember(app.name);
        continue;
      }
      found_but_not_target_apps.remove(app.uri);
      if (found_but_not_target_apps.empty()) {
        break;
      }
    }

    if (found_but_not_target_apps.empty()) {
      found_target = t;
      auto found_target_custom = found_target.custom_data();
      found_target_custom[Target::ComposeAppField] = shortlisted_target_apps;
      found_target.updateCustom(found_target_custom);
      break;
    }
  }

  return found_target;
}

static void registerApps(const Uptane::Target& target, const boost::filesystem::path& apps_store_root,
                         const boost::filesystem::path& docker_root) {
  const boost::filesystem::path repositories_file{docker_root / "/image/overlay2/repositories.json"};
  Json::Value repositories;
  if (boost::filesystem::exists(repositories_file)) {
    repositories = Utils::parseJSONFile(repositories_file.string());
  } else {
    repositories = Utils::parseJSON("{\"Repositories\":{}}");
  }

  for (const auto& app : Target::Apps(target)) {
    const Docker::Uri app_uri{Docker::Uri::parseUri(app.uri)};

    const auto app_dir{apps_store_root / "apps" / app_uri.app / app_uri.digest.hash()};
    if (!boost::filesystem::exists(app_dir)) {
      continue;
    }
    const auto app_compose_file{app_dir / Docker::RestorableAppEngine::ComposeFile};
    const Docker::ComposeInfo app_compose{app_compose_file.string()};

    for (const Json::Value& service : app_compose.getServices()) {
      const auto image_uri_str{app_compose.getImage(service)};
      const auto image_uri{Docker::Uri::parseUri(image_uri_str, false)};

      const auto image_index_path{app_dir / "images" / image_uri.registryHostname / image_uri.repo /
                                  image_uri.digest.hash() / "index.json"};
      const auto image_index{Utils::parseJSONFile(image_index_path.string())};

      // parse an image index to find a path to an image manifest
      const Docker::HashedDigest manifest_digest(image_index["manifests"][0]["digest"].asString());
      const auto image_manifest_path{apps_store_root / "blobs/sha256" / manifest_digest.hash()};
      const auto image_manifest{Utils::parseJSONFile(image_manifest_path)};
      // parse an image manifest to get a digest of an image config
      const Docker::HashedDigest config_digest(image_manifest["config"]["digest"].asString());
      const auto image_repo{image_uri.registryHostname + "/" + image_uri.repo};

      LOG_INFO << "Registering image: " << image_uri_str << " -> " << config_digest();
      repositories["Repositories"][image_repo][image_uri_str] = config_digest();
    }
  }
  Utils::writeFile(repositories_file.string(), repositories);
}

PostInstallAction install(const Config& cfg_in, const UpdateSrc& src,
                          std::shared_ptr<HttpInterface> docker_client_http_client) {
  auto client{createOfflineClient(cfg_in, src, docker_client_http_client)};

  const auto import{client->isRootMetaImportNeeded()};
  if (std::get<0>(import)) {
    // We don't know whether it's a production or CI device, so we just import the first two versions that are equal for
    // both prod and CI.
    // TODO: Consider the way to improve it if needed. For example:
    // 1) Introduce a new command `aklite-offline init <type> = (prod | ci)` that should be executed during device
    // manufacturing process 2) Determine a device type based on the first offline update content. If the first update
    // root metadata are production metadata, then
    //    consider a device as "prod" and import production root meta from a local file system, otherwise import CI
    //    meta.
    LOG_INFO << "Importing root metadata from a local file system...";
    if (client->importRootMeta(std::get<1>(import) / "ci", Uptane::Version(2))) {
      LOG_INFO << "Successfully imported root role metadata from " << std::get<1>(import) / "ci";
    } else {
      LOG_ERROR << "Failed to import root role metadata from " << std::get<1>(import) / "ci";
    }
  }

  auto rc = client->updateImageMeta();
  if (!std::get<0>(rc)) {
    throw std::runtime_error("Failed to pull TUF metadata: " + std::get<1>(rc));
  }

  auto target{getTarget(*client, src)};
  if (!target.IsValid()) {
    throw std::invalid_argument("Target to install has not been found");
  }

  LOG_INFO << "Found TUF Target that matches the given update content: " << target.filename();
  const auto download_res = client->download(target, "offline update to " + target.filename());
  if (!download_res) {
    throw std::runtime_error("Failed to download Target; err: " + download_res.description);
  }

  PostInstallAction post_install_action{PostInstallAction::Undefined};
  if (client->getCurrent().sha256Hash() != target.sha256Hash()) {
    const auto install_res = client->install(target);
    if (install_res != data::ResultCode::Numeric::kNeedCompletion) {
      throw std::runtime_error("Failed to install Target");
    }
    post_install_action = PostInstallAction::NeedReboot;
  } else if (client->config.pacman.type == ComposeAppManager::Name) {
    // don't `install` since it will create/run containers and we don't want to do it
    // before we register images and restart dockerd
    client->storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
    post_install_action = PostInstallAction::NeedDockerRestart;
  } else {
    post_install_action = PostInstallAction::AlreadyInstalled;
  }

  if (client->config.pacman.type == ComposeAppManager::Name) {
    const auto pacman_cfg{ComposeAppManager::Config(cfg_in.pacman)};
    registerApps(target, pacman_cfg.reset_apps_root, pacman_cfg.images_data_root);
  }

  return post_install_action;
}

PostRunAction run(const Config& cfg_in, std::shared_ptr<HttpInterface> docker_client_http_client) {
  auto client{createOfflineClient(cfg_in,
                                  /* src dir is not needed in the case of run command */
                                  {
                                      "unknown-tuf-dir",
                                      "unknown-ostree-dir",
                                      "unknown-apps-dir",
                                  },
                                  docker_client_http_client)};

  if (!client->checkImageMetaOffline()) {
    throw std::runtime_error("Invalid local TUF metadata");
  }

  boost::optional<Uptane::Target> pending;
  client->storage->loadInstalledVersions("", nullptr, &pending);
  if (!pending) {
    LOG_INFO << "No any pending installations found";
    return PostRunAction::Ok;
  }

  data::ResultCode::Numeric install_res{data::ResultCode::Numeric::kUnknown};
  const auto target{*pending};                     /* target to be applied and started */
  const auto current_target{client->getCurrent()}; /* current target */

  if (current_target.sha256Hash() != target.sha256Hash()) {
    // apply ostree installation and run Apps
    if (client->finalizeInstall()) {
      install_res = data::ResultCode::Numeric::kOk;
    } else {
      LOG_ERROR << "Failed to boot on the updated ostree-based rootfs or start updated Apps";
    }
  } else {
    // just run Apps of the new Target
    client->appsInSync(target);
    if ((install_res = client->install(target)) != data::ResultCode::Numeric::kOk) {
      LOG_ERROR << "Failed to start the updated Apps";
    }
  }

  if (install_res == data::ResultCode::Numeric::kOk && client->isTargetActive(target)) {
    if (client->config.pacman.type == ComposeAppManager::Name) {
      LOG_INFO << "Update has been successfully applied and Apps started: " << target.filename();
    } else {
      LOG_INFO << "Update has been successfully applied: " << target.filename();
    }
    return PostRunAction::Ok;
  }

  // Rollback
  data::ResultCode::Numeric rollback_install_res{data::ResultCode::Numeric::kUnknown};

  // If a device successfully booted to the new ostree version then there must be so-called rollback version of ostree.
  // if both ostree and Apps were updated but Apps failed to start after successfull boot then the rollback target
  // should be available.
  Uptane::Target rollback_target = client->getRollbackTarget();
  if (!rollback_target.IsValid()) {
    // If either a device failed to boot on a new device or ostree hasn't changed but new version Apps failed to start,
    // then the current version is actually "rollback" Target we need to switch to, which is effectivelly syncing Apps
    rollback_target = current_target;
  }

  LOG_INFO << "Rollback to " << rollback_target.filename();

  client->appsInSync(rollback_target);
  rollback_install_res = client->install(rollback_target);

  if (rollback_install_res != data::ResultCode::Numeric::kNeedCompletion &&
      rollback_install_res != data::ResultCode::Numeric::kOk) {
    LOG_ERROR << "Failed to rollback to: " << rollback_target.filename();
    LOG_ERROR << "Try to reboot and re-run";
    // we really don't know what to do in this case, just let user to reboot a device and let re-run again.
  }
  return (data::ResultCode::Numeric::kOk == rollback_install_res) ? PostRunAction::Ok
                                                                  : PostRunAction::RollbackNeedReboot;
}

}  // namespace client
}  // namespace offline
