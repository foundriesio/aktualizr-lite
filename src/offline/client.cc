#include "client.h"

#include <filesystem>

#include <boost/process.hpp>

#include "aktualizr-lite/api.h"
#include "appengine.h"
#include "composeapp/appengine.h"
#include "composeappmanager.h"
#include "docker/composeinfo.h"
#include "docker/docker.h"
#include "docker/restorableappengine.h"
#include "ostree/repo.h"
#include "storage/invstorage.h"
#include "target.h"

namespace fs = std::filesystem;

namespace offline {
namespace client {

static std::unique_ptr<LiteClient> createOfflineClient(
    const Config& cfg_in, const UpdateSrc& src, std::shared_ptr<HttpInterface> docker_client_http_client = nullptr);
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
    const auto blob_path{(blobs_dir_ / hash).string()};

    if (!boost::filesystem::exists(blob_path)) {
      return HttpResponse("The app blob is missing: " + blob_path, 404, CURLE_OK, "Not found");
    }

    char buf[1024 * 4];
    std::ifstream blob_file{blob_path};

    std::streamsize read_byte_numb;
    while (blob_file.good()) {
      blob_file.read(buf, sizeof(buf));
      write_cb(buf, blob_file.gcount(), 1, userp);
    }
    if (!blob_file.eof()) {
      HttpResponse("Failed to read app blob data: " + blob_path, 500, CURLE_OK, "Internal Error");
    }
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
    const auto blob_path{blobs_dir_ / hash};
    if (!boost::filesystem::exists(blob_path)) {
      return HttpResponse("The app blob is missing: " + blob_path.string(), 404, CURLE_OK, "Not found");
    }
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

static void setPacmanType(Config& cfg, bool no_custom_docker_client) {
  // Always use the compose app manager since it covers both use-cases, just ostree and ostree+apps,
  // unless a package manager type is enforced in the config.
  std::string type{ComposeAppManager::Name};
  // Unless there is no `docker` or `dockerd` and a custom docker client is not provided (e.g. by the unit test mock)
  if (no_custom_docker_client &&
      (!boost::filesystem::exists("/usr/bin/dockerd") || !boost::filesystem::exists("/usr/bin/docker"))) {
    type = RootfsTreeManager::Name;
  }
  if (cfg.pacman.extra.count("enforce_pacman_type") > 0) {
    type = cfg.pacman.extra.at("enforce_pacman_type");
    if (type != RootfsTreeManager::Name && type != ComposeAppManager::Name) {
      throw std::invalid_argument("unsupported package manager type: " + type);
    }
  }
  cfg.pacman.type = type;
}

static std::unique_ptr<LiteClient> createOfflineClient(const Config& cfg_in, const UpdateSrc& src,
                                                       std::shared_ptr<HttpInterface> docker_client_http_client) {
  Config cfg{cfg_in};  // make copy of the input config to avoid its modification by LiteClient

  // turn off reporting update events to DG
  cfg.tls.server = "";
  // make LiteClient to pull from a local ostree repo
  cfg.pacman.ostree_server = "file://" + src.OstreeRepoDir.string();
  setPacmanType(cfg, docker_client_http_client == nullptr);
  if (cfg.pacman.type == RootfsTreeManager::Name) {
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

#ifdef USE_COMPOSEAPP_ENGINE
  AppEngine::Ptr app_engine{std::make_shared<composeapp::AppEngine>(
      pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, registry_client,
      docker_client_http_client ? std::make_shared<Docker::DockerClient>(docker_client_http_client)
                                : std::make_shared<Docker::DockerClient>(),
      docker_host, compose_cmd, pacman_cfg.composectl_bin.string(), pacman_cfg.storage_watermark,
      Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
      [offline_registry](const Docker::Uri& app_uri, const std::string& image_uri) {
        Docker::Uri uri{Docker::Uri::parseUri(image_uri, false)};
        return "--src-shared-blob-dir " + offline_registry->blobsDir().string() +
               " oci:" + offline_registry->appsDir().string() + "/" + app_uri.app + "/" + app_uri.digest.hash() +
               "/images/" + uri.registryHostname + "/" + uri.repo + "/" + uri.digest.hash();
      },
      false, /* don't create containers on install because it makes dockerd check if pinned images
    present in its store what we should avoid until images are registered (hacked) in dockerd store */
      src.AppsDir.string())};
#else
  AppEngine::Ptr app_engine{std::make_shared<Docker::RestorableAppEngine>(
      pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, registry_client,
      docker_client_http_client ? std::make_shared<Docker::DockerClient>(docker_client_http_client)
                                : std::make_shared<Docker::DockerClient>(),
      pacman_cfg.skopeo_bin.string(), docker_host, compose_cmd, Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
      [offline_registry](const Docker::Uri& app_uri, const std::string& image_uri) {
        Docker::Uri uri{Docker::Uri::parseUri(image_uri, false)};
        return "--src-shared-blob-dir " + offline_registry->blobsDir().string() +
               " oci:" + offline_registry->appsDir().string() + "/" + app_uri.app + "/" + app_uri.digest.hash() +
               "/images/" + uri.registryHostname + "/" + uri.repo + "/" + uri.digest.hash();
      },
      false, /* don't create containers on install because it makes dockerd check if pinned images
    present in its store what we should avoid until images are registered (hacked) in dockerd store */
      true   /* indicate that this is an offline client */
      )};
#endif  // USE_COMPOSEAPP_ENGINE
  return std::make_unique<LiteClient>(cfg, app_engine, nullptr, std::make_shared<MetaFetcher>(src.TufDir));
}

static bool compareTargets(const Uptane::Target& t1, const Uptane::Target& t2) {
  return !(Target::Version(t1.custom_version()) < Target::Version(t2.custom_version()));
}

using SortedTargets = std::set<Uptane::Target, decltype(&compareTargets)>;

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

// Filter the specified list of Targets by hardware ID and sort them in descending version order
static SortedTargets filterAndSortTargets(const std::vector<Uptane::Target>& targets,
                                          const std::string& required_hwid) {
  SortedTargets res(compareTargets);

  for (const auto& target : targets) {
    if (target.hardwareIds().size() != 1) {
      LOG_ERROR << "Invalid hardware ID number found in Target; target: " << target.filename()
                << "; found: " << target.hardwareIds().size() << "; expected: " << 1;
      continue;
    }
    const auto hwid{target.hardwareIds()[0]};
    if (required_hwid != hwid.ToString()) {
      LOG_DEBUG << "Found Target's hardware ID doesn't match a device's hardware ID, skipping it; "
                << "target: " << target.filename() << " target hw ID: " << hwid << "; device hw ID: " << required_hwid;
      continue;
    }
    LOG_TRACE << "Found Target: " << target.filename();
    res.insert(target);
  }

  return res;
}

static void parseUpdateContent(const boost::filesystem::path& apps_dir, std::set<std::string>& found_apps) {
  if (!boost::filesystem::exists(apps_dir)) {
    return;
  }
  for (auto const& app_dir_entry : boost::filesystem::directory_iterator{apps_dir}) {
    const auto app_name{app_dir_entry.path().filename().string()};
    for (auto const& app_ver_dir_entry : boost::filesystem::directory_iterator{app_dir_entry.path()}) {
      const auto uri_file{app_ver_dir_entry.path() / "uri"};
      const auto app_uri{Utils::readFile(uri_file.string())};
      LOG_DEBUG << "Found app; uri: " << app_uri;
      found_apps.insert(app_uri);
    }
  }
}

static std::vector<Uptane::Target> getAvailableTargets(const PackageConfig& pconfig,
                                                       const SortedTargets& allowed_targets, const UpdateSrc& src,
                                                       bool just_latest = true) {
  if (allowed_targets.empty()) {
    LOG_ERROR << "No targets are available for a given device; check a hardware ID and/or a tag";
    return std::vector<Uptane::Target>{};
  }
  std::vector<Uptane::Target> found_targets;
  std::set<std::string> found_apps;

  parseUpdateContent(src.AppsDir / "apps", found_apps);
  LOG_INFO << "Apps found in the source directory " << src.AppsDir;
  for (const auto& app : found_apps) {
    LOG_INFO << "\t" << app;
  }

  const OSTree::Repo repo{src.OstreeRepoDir.string()};
  Uptane::Target found_target(Uptane::Target::Unknown());

  const std::string search_msg{just_latest ? "a target" : "all targets"};
  LOG_INFO << "Searching for " << search_msg << " starting from " << allowed_targets.begin()->filename()
           << " that match content provided in the source directory\n"
           << "\tpacman type: \t" << pconfig.type << "\n\t apps dir: \t" << src.AppsDir << "\n\t ostree dir: \t"
           << src.OstreeRepoDir;
  for (const auto& t : allowed_targets) {
    LOG_INFO << "Checking " << t.filename();
    if (!repo.hasCommit(t.sha256Hash())) {
      LOG_INFO << "\tmissing ostree commit: " << t.sha256Hash();
      continue;
    }
    if (pconfig.type != ComposeAppManager::Name) {
      found_targets.emplace_back(t);
      LOG_INFO << "\tall target content have been found";
      if (!just_latest) {
        continue;
      }
      break;
    }
    const ComposeAppManager::AppsContainer required_apps{
        ComposeAppManager::getRequiredApps(ComposeAppManager::Config(pconfig), t)};
    std::set<std::string> missing_apps;
    Json::Value apps_to_install;
    for (const auto& app : required_apps) {
      if (found_apps.count(app.second) == 0) {
        missing_apps.insert(app.second);
      } else {
        apps_to_install[app.first]["uri"] = app.second;
      }
    }
    if (!missing_apps.empty()) {
      LOG_INFO << "\tmissing apps:";
      for (const auto& app : missing_apps) {
        LOG_INFO << "\t\t" << app;
      }
      continue;
    }
    Json::Value updated_custom{t.custom_data()};
    updated_custom[Target::ComposeAppField] = apps_to_install;
    found_targets.emplace_back(Target::updateCustom(t, updated_custom));
    LOG_INFO << "\tall target content have been found";
    if (just_latest) {
      break;
    }
  }
  return found_targets;
}

static Uptane::Target getTarget(LiteClient& client, const UpdateSrc& src) {
  if (!src.TargetName.empty()) {
    return getSpecificTarget(client, src.TargetName);
  }
  const auto available_targets{getAvailableTargets(
      client.config.pacman, filterAndSortTargets(client.allTargets(), client.primary_ecu.second.ToString()), src)};
  if (available_targets.empty()) {
    return Uptane::Target::Unknown();
  }
  return available_targets.front();
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

      // The image "registration" is not needed since LmP v92 because of the docker patch that
      // register image just afet it is been laoded to the docker store.
      // However, we want to keep image registration anyway since a user may do downgrade,
      // so the `run` command can be execute in the LmP < v92 which does not include the docker patch.
      LOG_DEBUG << "Registering image: " << image_uri_str << " -> " << config_digest();
      repositories["Repositories"][image_repo][image_uri_str] = config_digest();
    }
  }
  Utils::writeFile(repositories_file.string(), repositories);
}

static void shortlistTargetAppsByContent(const boost::filesystem::path& apps_root, Uptane::Target& target) {
  if (!boost::filesystem::exists(apps_root) || !boost::filesystem::exists(apps_root / "apps")) {
    return;
  }
  const auto target_apps{Target::appsJson(target)};
  if (target_apps.empty()) {
    return;
  }
  const fs::path apps_dir{(apps_root / "apps").string()};
  Json::Value app_shortlist;
  for (auto const& app_dir_entry : std::filesystem::directory_iterator{apps_dir}) {
    const auto app_name{app_dir_entry.path().filename().string()};
    if (!target_apps.isMember(app_name)) {
      continue;
    }
    for (auto const& app_ver_dir_entry : std::filesystem::directory_iterator{app_dir_entry.path()}) {
      const auto uri_file{app_ver_dir_entry.path() / "uri"};
      if (fs::exists(uri_file)) {
        const auto app_uri{Utils::readFile(uri_file.string())};
        if (target_apps[app_name]["uri"] == app_uri) {
          app_shortlist[app_name]["uri"] = app_uri;
        }
      }
    }
  }
  auto custom{target.custom_data()};
  custom[Target::ComposeAppField] = app_shortlist;
  target.updateCustom(custom);
}

PostInstallAction install(const Config& cfg_in, const UpdateSrc& src,
                          std::shared_ptr<HttpInterface> docker_client_http_client, bool force_downgrade) {
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
  const auto current{client->getCurrent()};
  if (Target::Version(current.custom_version()) > Target::Version(target.custom_version())) {
    LOG_WARNING << "Found TUF Target is lower version than the current on; "
                << "current: " << current.custom_version() << ", found Target: " << target.custom_version();

    if (!force_downgrade) {
      LOG_ERROR << "Downgrade is not allowed by default, re-run the command with `--force` option to force downgrade";
      return PostInstallAction::DowngradeAttempt;
    }
    LOG_WARNING << "Downgrading from " << current.custom_version() << " to  " << target.custom_version() << "...";
  }

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
    if (client->isPendingTarget(target)) {
      post_install_action = PostInstallAction::NeedReboot;
    } else {
      post_install_action = PostInstallAction::NeedRebootForBootFw;
    }
  } else if (client->config.pacman.type != ComposeAppManager::Name || client->appsInSync(target)) {
    post_install_action = PostInstallAction::AlreadyInstalled;
  } else {
    // just download apps in the case of "app only" update
    client->storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
    post_install_action = PostInstallAction::Ok;
  }

  if (client->config.pacman.type == ComposeAppManager::Name &&
      post_install_action != PostInstallAction::AlreadyInstalled) {
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
    LOG_INFO << "No pending installations found";
    return PostRunAction::OkNoPendingInstall;
  }

  data::ResultCode::Numeric install_res{data::ResultCode::Numeric::kUnknown};
  const auto target{*pending};                     /* target to be applied and started */
  const auto current_target{client->getCurrent()}; /* current target */

  if (client->finalizeInstall()) {
    install_res = data::ResultCode::Numeric::kOk;
  } else {
    LOG_ERROR << "Failed to boot on the updated ostree-based rootfs or start updated Apps";
  }

  if (install_res == data::ResultCode::Numeric::kOk && client->isTargetActive(target)) {
    if (client->config.pacman.type == ComposeAppManager::Name) {
      LOG_INFO << "Update has been successfully applied and Apps started: " << target.filename();
    } else {
      LOG_INFO << "Update has been successfully applied: " << target.filename();
    }
    return client->isBootFwUpdateInProgress() ? PostRunAction::OkNeedReboot : PostRunAction::Ok;
  }

  // Rollback
  data::ResultCode::Numeric rollback_install_res{data::ResultCode::Numeric::kUnknown};

  // If a device successfully booted to the new ostree version then there must be so-called rollback version of ostree.
  // if both ostree and Apps were updated but Apps failed to start after successfull boot then the rollback target
  // should be available.
  Uptane::Target rollback_target = client->getRollbackTarget();
  if (!rollback_target.IsValid()) {
    if (Target::isUnknown(current_target)) {
      // We don't know details of TUF Target to rollback to, so just let device to stay on the current undefined state
      LOG_ERROR << "Device rollbacked to the previous Target's rootfs/ostree; "
                << " TUF Target to rollback to is unknown so cannot sync Apps to desired state.";
      return PostRunAction::RollbackToUnknown;
    }
    if (client->isRollback(current_target)) {
      // We don't know details of TUF Target to rollback to, and already booted on "failing" Target
      LOG_ERROR << "Failed to start the updated Apps after successfull boot on the updated rootfs;"
                   " TUF Target to rollback to is unknown so cannot perform a rollback to the previous version.";
      // Should we deploy the rollback ostree hash and advise a client to reboot to complete the rollback even if
      // the rollback target is "unknown" just an ostree rollback hash is known?
      return PostRunAction::RollbackToUnknownIfAppFailed;
    }
    // If either a device failed to boot on a new device or ostree hasn't changed but new version Apps failed to start,
    // then the current version is actually "rollback" Target we need to switch to, which is effectivelly syncing Apps
    rollback_target = current_target;
  }

  ComposeAppManager::Config pacman_cfg(cfg_in.pacman);
  shortlistTargetAppsByContent(pacman_cfg.reset_apps_root, rollback_target);
  if (current_target.sha256Hash() == rollback_target.sha256Hash()) {
    client->logTarget("Syncing with Target that device rollbacked to:  ", rollback_target);
    // The Apps we rollbacked to must be started, and the new version Apps/blobs should be removed.
    // Effectively, we have to perform the same finalization as the one is done just after reboot on a new version.
    client->storage->savePrimaryInstalledVersion(rollback_target, InstalledVersionUpdateMode::kPending);
    if (!client->finalizeInstall()) {
      LOG_ERROR << "Failed to start the rollback Target Apps";
      return PostRunAction::RollbackFailed;
    }
    rollback_install_res = data::ResultCode::Numeric::kOk;
  } else {
    client->logTarget("Rollbacking to: ", rollback_target);
    client->appsInSync(rollback_target);
    rollback_install_res = client->install(rollback_target);
  }

  if (rollback_install_res != data::ResultCode::Numeric::kNeedCompletion &&
      rollback_install_res != data::ResultCode::Numeric::kOk) {
    LOG_ERROR << "Failed to rollback to: " << rollback_target.filename();
    LOG_ERROR << "Try to reboot and re-run";
    // we really don't know what to do in this case, just let user to reboot a device and let re-run again.
  }
  return (data::ResultCode::Numeric::kOk == rollback_install_res) ? PostRunAction::RollbackOk
                                                                  : PostRunAction::RollbackNeedReboot;
}

const Uptane::Target getCurrent(const Config& cfg_in, std::shared_ptr<HttpInterface> docker_client_http_client) {
  auto client{createOfflineClient(cfg_in,
                                  /* src dir is not needed in the case of run command */
                                  {
                                      "unknown-tuf-dir",
                                      "unknown-ostree-dir",
                                      "unknown-apps-dir",
                                  },
                                  docker_client_http_client)};
  return client->getCurrent();
}

std::vector<Uptane::Target> check(const Config& cfg_in, const UpdateSrc& src) {
  const auto targets_json{Utils::parseJSONFile(src.TufDir / "targets.json")};
  const Uptane::Targets targets{targets_json};
  Config cfg{cfg_in};
  setPacmanType(cfg, true);
  return getAvailableTargets(cfg.pacman, filterAndSortTargets(targets.targets, cfg.provision.primary_ecu_hardware_id),
                             src, false);
}

}  // namespace client
}  // namespace offline
