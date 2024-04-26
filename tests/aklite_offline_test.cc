#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>
#include <fstream>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "aktualizr-lite/cli/cli.h"
#include "appengine.h"
#include "composeapp/appengine.h"
#include "composeappmanager.h"
#include "liteclient.h"
#include "target.h"
#include "tuf/akrepo.h"

// include test fixtures
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/liteclient/boot_flag_mgr.cc"
#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"
#include "fixtures/liteclient/sysostreerepomock.cc"
#include "fixtures/liteclient/sysrootfs.cc"
#include "fixtures/liteclient/tufrepomock.cc"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

// Defined in fstatvfs-mock.cc
extern void SetFreeBlockNumb(uint64_t, uint64_t);
extern void UnsetFreeBlockNumb();

class LiteClientMock : public LiteClient {
 public:
  LiteClientMock(Config& config_in, const std::shared_ptr<AppEngine>& app_engine = nullptr)
      : LiteClient(config_in, app_engine) {}

  MOCK_METHOD(void, callback, (const char* msg, const Uptane::Target& install_target, const std::string& result),
              (override));
};

class AppStore {
 public:
  AppStore(const boost::filesystem::path& root_dir, const std::string& hostname = "hub.foundries.io")
      : root_dir_{root_dir}, hostname_{hostname} {}

  AppEngine::App addApp(const fixtures::ComposeApp::Ptr& app) {
    const auto app_dir{apps_dir_ / app->name() / app->hash()};

    // store App data
    boost::filesystem::create_directories(app_dir);
    Utils::writeFile(app_dir / "manifest.json", app->manifest());
    Utils::writeFile(blobs_dir_ / app->hash(), app->manifest());
    Utils::writeFile(app_dir / (app->archHash() + ".tgz"), app->archive());
    Utils::writeFile(blobs_dir_ / app->archHash(), app->archive());
    Utils::writeFile(blobs_dir_ / app->layersHash(), app->layersManifest());

    // store image in the skopeo/OCI compliant way
    const auto image_uri{app->image().uri()};
    Docker::Uri uri{Docker::Uri::parseUri(image_uri)};
    const auto image_dir{app_dir / "images" / uri.registryHostname / uri.repo / uri.digest.hash()};
    boost::filesystem::create_directories(image_dir);
    Utils::writeFile(image_dir / "oci-layout", std::string("{\"imageLayoutVersion\": \"1.0.0\"}"));

    Json::Value index_json;
    index_json["schemaVersion"] = 2;
    index_json["manifests"][0]["mediaType"] = "application/vnd.docker.distribution.manifest.v2+json";
    index_json["manifests"][0]["digest"] = "sha256:" + app->image().manifest().hash;
    index_json["manifests"][0]["size"] = app->image().manifest().size;
    index_json["manifests"][0]["platform"]["architecture"] = "amd64";
    index_json["manifests"][0]["platform"]["os"] = "linux";
    Utils::writeFile(image_dir / "index.json", Utils::jsonToStr(index_json));
    Utils::writeFile(blobs_dir_ / app->image().manifest().hash, app->image().manifest().data);
    Utils::writeFile(blobs_dir_ / app->image().config().hash, app->image().config().data);
    Utils::writeFile(blobs_dir_ / app->image().layerBlob().hash, app->image().layerBlob().data);

    const auto app_uri{hostname_ + '/' + "factory/" + app->name() + '@' + "sha256:" + app->hash()};
    Utils::writeFile(app_dir / "uri", app_uri);
    return {app->name(), app_uri};
  }

  boost::filesystem::path blobsDir() const { return root_dir_ / "blobs"; }
  const boost::filesystem::path& appsDir() const { return apps_dir_; }
  const boost::filesystem::path& dir() const { return root_dir_; }

 private:
  const boost::filesystem::path root_dir_;
  const std::string hostname_;
  const boost::filesystem::path apps_dir_{root_dir_ / "apps"};
  const boost::filesystem::path blobs_dir_{root_dir_ / "blobs" / "sha256"};
};

class AkliteOffline : public ::testing::Test {
 protected:
  AkliteOffline()
      : sys_rootfs_{(test_dir_.Path() / "sysroot-fs").string(), branch, hw_id, os},
        sys_repo_{(test_dir_.Path() / "sysrepo").string(), os},
        ostree_repo_{(test_dir_.Path() / "treehub").string(), true},
        tuf_repo_{src_dir_ / "tuf"},
        daemon_{test_dir_.Path() / "daemon"},
        app_store_{test_dir_.Path() / "apps"},
        boot_flag_mgr_{std::make_shared<FioVb>((test_dir_.Path() / "fiovb").string())} {
    // hardware ID
    cfg_.provision.primary_ecu_hardware_id = hw_id;
    cfg_.provision.primary_ecu_serial = "test_primary_ecu_serial_id";

    // a path to the config dir
    cfg_.storage.path = test_dir_.Path() / "sota-dir";

    // ostree-based sysroot config
    cfg_.pacman.sysroot = sys_repo_.getPath();
    cfg_.pacman.os = os;
    cfg_.pacman.booted = BootedType::kStaged;

    // configure bootloader/booting related functionality
    cfg_.bootloader.reboot_command = "/bin/true";
    cfg_.bootloader.reboot_sentinel_dir = test_dir_.Path();
    cfg_.bootloader.rollback_mode = RollbackMode::kFioVB;

    cfg_.pacman.extra["tags"] = "default-tag";
    cfg_.pacman.extra["reset_apps"] = "";
    cfg_.pacman.extra["reset_apps_root"] = (test_dir_.Path() / "reset-apps").string();
    cfg_.pacman.extra["compose_apps_root"] = (test_dir_.Path() / "compose-apps").string();
    cfg_.pacman.extra["docker_compose_bin"] =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";
    cfg_.pacman.extra["images_data_root"] = daemon_.dataRoot();

    cfg_.import.base_path = cfg_.storage.path / "import";

    // add an initial rootfs to the ostree repo (treehub), pull it to the sysroot's repo, and deploy it
    const auto hash{ostree_repo_.commit(sys_rootfs_.path, sys_rootfs_.branch)};
    sys_repo_.getRepo().pullLocal(ostree_repo_.getPath(), hash);
    sys_repo_.deploy(hash);
    setInitialTarget(hash);
    docker_client_ = std::make_shared<Docker::DockerClient>(daemon_.getClient());
    local_update_source_ = {tuf_repo_.getRepoPath(), ostree_repo_.getPath(), app_store_.dir().string(),
                            reinterpret_cast<void*>(&docker_client_)};
  }

  void SetUp() {
    auto env{boost::this_process::environment()};
    env.set("DOCKER_HOST", daemon_.getUrl());
    SetFreeBlockNumb(90, 100);
  }

  void TearDown() { UnsetFreeBlockNumb(); }

  AppEngine::Ptr createAppEngine() {
    // Handle DG:/token-auth
    std::shared_ptr<HttpInterface> registry_basic_auth_client{nullptr};
    ComposeAppManager::Config pacman_cfg(cfg_.pacman);
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

    auto docker_client{local_update_source_.docker_client_ptr != nullptr
                           ? *(reinterpret_cast<Docker::DockerClient::Ptr*>(local_update_source_.docker_client_ptr))
                           : std::make_shared<Docker::DockerClient>()};

#ifdef USE_COMPOSEAPP_ENGINE
    AppEngine::Ptr app_engine{std::make_shared<composeapp::AppEngine>(
        pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, nullptr, docker_client,
        docker_host, compose_cmd, pacman_cfg.composectl_bin.string(), pacman_cfg.storage_watermark,
        Docker::RestorableAppEngine::GetDefStorageSpaceFunc(), nullptr,
        false, /* don't create containers on install because it makes dockerd check if pinned images
      present in its store what we should avoid until images are registered (hacked) in dockerd store */
        local_update_source_.app_store)};
#else
    AppEngine::Ptr app_engine{std::make_shared<Docker::RestorableAppEngine>(
        pacman_cfg.reset_apps_root, pacman_cfg.apps_root, pacman_cfg.images_data_root, registry_client, docker_client,
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
#endif

    return app_engine;
  }

  std::shared_ptr<testing::NiceMock<LiteClientMock>> createLiteClient() {
    if (cfg_.pacman.type != RootfsTreeManager::Name) {
      return std::make_shared<testing::NiceMock<LiteClientMock>>(cfg_, createAppEngine());
    } else {
      return std::make_shared<testing::NiceMock<LiteClientMock>>(cfg_);
    }
  }

  std::vector<TufTarget> check() {
    AkliteClient client(createLiteClient());
    const CheckInResult cr{client.CheckInLocal(src())};
    if (!cr) {
      throw std::runtime_error("failed to checkin offline update");
    }
    return cr.Targets();
  }

  aklite::cli::StatusCode install() {
    auto liteClient = createLiteClient();
    AkliteClient client(liteClient);
    return aklite::cli::Install(client, -1, "", InstallMode::OstreeOnly, false, src());
  }

  aklite::cli::StatusCode run() {
    AkliteClient client(createLiteClient());
    return aklite::cli::CompleteInstall(client);
  }

  bool areAppsInSync() {
    AkliteClient client(createLiteClient());
    return client.CheckAppsInSync() == nullptr;
  }

  const TufTarget getCurrent() {
    AkliteClient client(createLiteClient());
    return client.GetCurrent();
  }

  void setInitialTarget(const std::string& hash, bool known = true, Json::Value* custom_data = nullptr) {
    Uptane::EcuMap ecus{{Uptane::EcuSerial("test_primary_ecu_serial_id"), Uptane::HardwareIdentifier(hw_id)}};
    std::vector<Hash> hashes{Hash(Hash::Type::kSha256, hash)};
    Uptane::Target initial_target =
        Uptane::Target{known ? hw_id + "-lmp-1" : Target::InitialTarget, ecus, hashes, 0, "", "OSTREE"};
    // update the initial Target to add the hardware ID so Target::MatchTarget() works correctly
    if (known && custom_data != nullptr) {
      initial_target.updateCustom(*custom_data);
    } else {
      auto custom{initial_target.custom_data()};
      custom["hardwareIds"][0] = cfg_.provision.primary_ecu_hardware_id;
      custom["version"] = "1";
      initial_target.updateCustom(custom);
    }
    // set initial bootloader version
    const auto boot_fw_ver{bootloader::BootloaderLite::getVersion(sys_repo_.getDeploymentPath(), hash)};
    boot_flag_mgr_->set("bootfirmware_version", boot_fw_ver);
    initial_target_ = Target::toTufTarget(initial_target);
  }

  TufTarget addTarget(TufRepoMock& repo, const std::vector<AppEngine::App>& apps, bool just_apps = false,
                      bool add_bootloader_update = false, const std::string& ci_app_shortlist = "") {
    const auto& latest_target{repo.getLatest()};
    std::string version;
    try {
      version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
    } catch (...) {
      LOG_INFO << "No target available, preparing the first version";
      version = "2";
    }
    auto hash{latest_target.IsValid() ? latest_target.sha256Hash() : initial_target_.Sha256Hash()};
    if (!just_apps) {
      // update rootfs and commit it into Treehub's repo
      const std::string unique_content = Utils::randomUuid();
      const std::string unique_file = Utils::randomUuid();
      Utils::writeFile(sys_rootfs_.path + "/" + unique_file, unique_content, true);
      if (add_bootloader_update) {
        Utils::writeFile(sys_rootfs_.path + bootloader::BootloaderLite::VersionFile,
                         std::string("bootfirmware_version=111"), true);
      }
      hash = ostree_repo_.commit(sys_rootfs_.path, branch);
    }
    Json::Value apps_json;
    for (const auto& app : apps) {
      apps_json[app.name]["uri"] = app.uri;
    }
    // add new target to TUF repo
    const std::string name = hw_id_ + "-" + os + "-" + version;
    auto target{
        Target::toTufTarget(repo.addTarget(name, hash, hw_id_, version, apps_json, Json::Value(), ci_app_shortlist))};
    repo.updateBundleMeta(target.Name());
    return target;
  }

  TufTarget addTarget(const std::vector<AppEngine::App>& apps, bool just_apps = false,
                      bool add_bootloader_update = false, const std::string& ci_app_shortlist = "") {
    return addTarget(tuf_repo_, apps, just_apps, add_bootloader_update, ci_app_shortlist);
  }

  void preloadApps(const std::vector<AppEngine::App>& apps, const std::vector<std::string>& apps_not_to_preload,
                   bool add_installed_versions = true) {
    // do App preloading by installing Target that refers to the current ostree hash
    // and contains the list of apps to preload.
    TufTarget preloaded_target{initial_target_};
    Json::Value apps_json;

    std::set<std::string> apps_to_shortlist;
    for (const auto& app : apps) {
      apps_json[app.name]["uri"] = app.uri;
      apps_to_shortlist.emplace(app.name);
    }
    tuf_repo_.addTarget(cfg_.provision.primary_ecu_hardware_id + "-lmp-1", initial_target_.Sha256Hash(),
                        cfg_.provision.primary_ecu_hardware_id, "1", apps_json);
    tuf_repo_.updateBundleMeta(preloaded_target.Name());

    // content-based shortlisting
    for (const auto& app : apps_not_to_preload) {
      boost::filesystem::remove_all(app_store_.appsDir() / app);
      apps_to_shortlist.erase(app);
    }
    setAppsShortlist(boost::algorithm::join(apps_to_shortlist, ","));
    ASSERT_EQ(aklite::cli::StatusCode::InstallAppsNeedFinalization, install());
    ASSERT_EQ(aklite::cli::StatusCode::Ok, run());

    if (add_installed_versions) {
      Json::Value installed_target_json;
      installed_target_json[initial_target_.Name()]["hashes"]["sha256"] = initial_target_.Sha256Hash();
      installed_target_json[initial_target_.Name()]["length"] = 0;
      installed_target_json[initial_target_.Name()]["is_current"] = true;
      Json::Value custom;
      custom[Target::ComposeAppField] = apps_json;
      custom["name"] = cfg_.provision.primary_ecu_hardware_id + "-lmp";
      custom["version"] = "1";
      custom["hardwareIds"][0] = cfg_.provision.primary_ecu_hardware_id;
      custom["targetFormat"] = "OSTREE";
      custom["arch"] = "arm64";
      installed_target_json[initial_target_.Name()]["custom"] = custom;
      Utils::writeFile(cfg_.import.base_path / "installed_versions", installed_target_json);
      // ASSERT_EQ(getCurrent().filename(), initial_target_.filename());
      setInitialTarget(initial_target_.Sha256Hash(), true, &custom);
    } else {
      // we need to remove the initial target from the TUF repo so it's not listed in the source TUF repo
      // for the following offline update and as result it will be "unknown" to the client.
      tuf_repo_.reset();
      // Turn the "known" target to the "initial" since it's not in DB/TUF meta
      setInitialTarget(initial_target_.Sha256Hash(), false);
    }
    // remove the DB generated during the update for the app preloading, to emulate real-life situation
    boost::filesystem::remove(cfg_.storage.sqldb_path.get(cfg_.storage.path));
  }

  const std::string getSentinelFilePath() const {
    return (cfg_.bootloader.reboot_sentinel_dir / "need_reboot").string();
  }

  void reboot() {
    boost::filesystem::remove(getSentinelFilePath());
    reloadDockerEngine();
  }

  void reloadDockerEngine() {
    // emulate registration of images located in `/var/lib/docker/image/overlay2/repositories.json
    const boost::filesystem::path repositories_file{daemon_.dir() / "/image/overlay2/repositories.json"};
    Json::Value repositories;
    Json::Value images{Utils::parseJSONFile(daemon_.dir() / "images.json")};
    if (boost::filesystem::exists(repositories_file)) {
      repositories = Utils::parseJSONFile(repositories_file.string());
    } else {
      repositories = Utils::parseJSON("{\"Repositories\":{}}");
    }

    for (const auto& repo : repositories["Repositories"]) {
      for (Json::ValueConstIterator image_it = repo.begin(); image_it != repo.end(); ++image_it) {
        const auto image_uri{image_it.key().asString()};
        images[image_uri] = true;
      }
    }
    // The docker daemon and docker compose mocks use the "image.json" file
    Utils::writeFile(daemon_.dir() / "images.json", images);
  }

  AppEngine::App createApp(const std::string& name, const std::string& failure = "none") {
    const auto layer_size{1024};
    Json::Value layers;
    layers["layers"][0]["digest"] =
        "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
    layers["layers"][0]["size"] = layer_size;

    return app_store_.addApp(fixtures::ComposeApp::createAppWithCustomeLayers(name, layers, boost::none, failure));
  }

  const LocalUpdateSource* src() const { return &local_update_source_; }

  void setAppsShortlist(const std::string& shortlist) { cfg_.pacman.extra["compose_apps"] = shortlist; }
  void setTargetHwId(const std::string& hw_id) { hw_id_ = hw_id; }

 protected:
  static const std::string hw_id;
  static const std::string os;
  static const std::string branch;

 protected:
  TemporaryDirectory test_dir_;  // must be the first element in the class
  const boost::filesystem::path src_dir_{test_dir_.Path() / "offline-update-src"};
  Config cfg_;
  SysRootFS sys_rootfs_;        // a sysroot that is bitbaked and added to the ostree repo that liteclient fetches from
  SysOSTreeRepoMock sys_repo_;  // an ostree-based sysroot that liteclient manages
  OSTreeRepoMock ostree_repo_;  // a source ostree repo to fetch update from
  TufRepoMock tuf_repo_;
  fixtures::DockerDaemon daemon_;
  AppStore app_store_;
  BootFlagMgr::Ptr boot_flag_mgr_;
  TufTarget initial_target_;
  Docker::DockerClient::Ptr docker_client_;
  LocalUpdateSource local_update_source_;
  std::string hw_id_{hw_id};
};

const std::string AkliteOffline::hw_id{"raspberrypi4-64"};
const std::string AkliteOffline::os{"lmp"};
const std::string AkliteOffline::branch{hw_id + "-" + os};

TEST_F(AkliteOffline, OfflineClientInvalidBundleMeta) {
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto target{addTarget({createApp("app-01")})};
  auto lite_cli = createLiteClient();
  AkliteClient client(lite_cli);

  // invalidate the bundle metadata signature
  Json::Value bundle_meta{Utils::parseJSONFile(tuf_repo_.getBundleMetaPath())};
  bundle_meta["signed"]["foo"] = "bar";
  Utils::writeFile(tuf_repo_.getBundleMetaPath(), bundle_meta);

  const CheckInResult cr{client.CheckInLocal(src())};
  ASSERT_EQ(CheckInResult::Status::BundleMetadataError, cr.status);

  ASSERT_EQ(aklite::cli::StatusCode::CheckinInvalidBundleMetadata, aklite::cli::CheckIn(client, src()));
  ASSERT_EQ(aklite::cli::StatusCode::CheckinInvalidBundleMetadata,
            aklite::cli::Install(client, -1, "", InstallMode::All, false, src()));
}

TEST_F(AkliteOffline, OfflineClientCheckinSecurityError) {
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto outdated_repo_path{test_dir_ / "outdated_tuf_repo"};
  boost::filesystem::copy(tuf_repo_.getRepoPath(), outdated_repo_path);
  const auto target{addTarget({createApp("app-01")})};

  // Do checkin to update the tuf repo with up-to-date TUF meta
  const auto available_targets{check()};
  ASSERT_EQ(2, available_targets.size());

  // Now try to do checkin against the outdated TUF repo
  AkliteClient client(createLiteClient());
  LocalUpdateSource outdated_src = {outdated_repo_path.string(), src()->ostree_repo, src()->app_store};
  ASSERT_EQ(aklite::cli::StatusCode::CheckinOkCached, aklite::cli::CheckIn(client, &outdated_src));
}

TEST_F(AkliteOffline, OfflineClientCheckinMetadataNotFound) {
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto target{addTarget({createApp("app-01")})};
  const auto invalid_repo_path{test_dir_ / "invalid_tuf_repo"};

  // Now try to do checkin against an invalid TUF repo path
  AkliteClient client(createLiteClient());
  LocalUpdateSource invalid_src = {invalid_repo_path.string(), src()->ostree_repo, src()->app_store};
  ASSERT_EQ(aklite::cli::StatusCode::CheckinMetadataNotFound, aklite::cli::CheckIn(client, &invalid_src));
}

TEST_F(AkliteOffline, OfflineClientCheckinExpiredMetadata) {
  TufRepoMock expired_repo{src_dir_ / "tuf", "2010-01-01T00:00:00Z"};

  const auto prev_target{addTarget(expired_repo, {createApp("app-01")})};
  const auto target{addTarget(expired_repo, {createApp("app-01")})};

  AkliteClient client(createLiteClient());
  ASSERT_EQ(aklite::cli::StatusCode::CheckinExpiredMetadata, aklite::cli::CheckIn(client, src()));
}

TEST_F(AkliteOffline, OfflineClientCheckinCheckinNoMatchingTargets) {
  AkliteClient client(createLiteClient());

  setTargetHwId("some-other-hw-id");
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto target{addTarget({createApp("app-01")})};

  ASSERT_EQ(aklite::cli::StatusCode::CheckinNoMatchingTargets, aklite::cli::CheckIn(client, src()));
}

TEST_F(AkliteOffline, OfflineClientCheckinCheckinNoTargetContent) {
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto target{addTarget({createApp("app-01")})};

  boost::filesystem::remove_all(app_store_.appsDir() / "app-01");
  AkliteClient client(createLiteClient());
  ASSERT_EQ(aklite::cli::StatusCode::CheckinNoTargetContent, aklite::cli::CheckIn(client, src()));
}

TEST_F(AkliteOffline, OfflineClient) {
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto target{addTarget({createApp("app-01")})};
  {
    auto lite_cli = createLiteClient();
    AkliteClient client(lite_cli);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("check-for-update-pre"), testing::_, testing::StrEq(""))).Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("check-for-update-post"), testing::_, testing::StrEq("OK")))
        .Times(1);
    const CheckInResult cr{client.CheckInLocal(src())};
    ASSERT_TRUE(cr);
    const auto available_targets = cr.Targets();

    ASSERT_EQ(2, available_targets.size());
    ASSERT_EQ(target, available_targets.back());

    // Install operation calls CheckInLocal as well
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("check-for-update-pre"), testing::_, testing::StrEq(""))).Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("check-for-update-post"), testing::_, testing::StrEq("OK")))
        .Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("download-pre"), testing::_, testing::StrEq(""))).Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("download-post"), testing::_, testing::StrEq("OK"))).Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("install-pre"), testing::_, testing::StrEq(""))).Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("install-post"), testing::_, testing::StrEq("NEEDS_COMPLETION")))
        .Times(1);
    ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot,
              aklite::cli::Install(client, -1, "", InstallMode::OstreeOnly, false, src()));
    reboot();
  }
  {
    auto lite_cli = createLiteClient();
    AkliteClient client(lite_cli);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("install-final-pre"), testing::_, testing::StrEq(""))).Times(1);
    EXPECT_CALL(*lite_cli, callback(testing::StrEq("install-post"), testing::_, testing::StrEq("OK"))).Times(1);
    ASSERT_EQ(aklite::cli::StatusCode::Ok, aklite::cli::CompleteInstall(client));
    ASSERT_EQ(target, getCurrent());
    ASSERT_TRUE(areAppsInSync());
  }
}

TEST_F(AkliteOffline, OfflineClientInstallNotLatest) {
  const auto target{addTarget({createApp("app-01")})};
  const auto app01_updated{createApp("app-01")};
  const auto latest_target{addTarget({app01_updated})};
  const auto app01_updated_uri{Docker::Uri::parseUri(app01_updated.uri)};
  boost::filesystem::remove_all(app_store_.appsDir() / app01_updated.name / app01_updated_uri.digest.hash());

  const auto available_targets{check()};
  ASSERT_EQ(1, available_targets.size());
  ASSERT_EQ(target, available_targets.back());
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(target, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, OfflineClientMultipleTargets) {
  const std::vector<TufTarget> targets{addTarget({createApp("app-01")}),
                                       addTarget({createApp("app-01"), createApp("app-02")}),
                                       addTarget({createApp("app-02"), createApp("app-03")})};

  const auto found_targets{check()};
  ASSERT_EQ(targets.size(), found_targets.size());
  for (int ii = 0; ii < targets.size(); ++ii) {
    ASSERT_EQ(targets[ii], found_targets[ii]);
  }
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(targets[targets.size() - 1], found_targets[found_targets.size() - 1]);
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, OfflineClientShortlistedApps) {
  const auto app03{createApp("zz00-app-03")};
  const auto target{addTarget({createApp("app-01"), createApp("app-02"), app03})};
  // remove zz00-app-03 (app03) from the file system to make sure that offline update succeeds
  // if one of the targets apps is missing in the provided update content and the corresponding app shortlist
  // is set in the client config.
  boost::filesystem::remove_all(app_store_.appsDir() / app03.name);
  setAppsShortlist("app-01, app-02");

  const auto available_targets{check()};
  ASSERT_EQ(1, available_targets.size());
  ASSERT_EQ(target, available_targets.back());
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(target, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, OfflineClientShortlistedAppsInCI) {
  const auto app02{createApp("app-02")};
  const auto app03{createApp("zz00-app-03")};
  const auto target{addTarget({createApp("app-01"), app02, app03}, false, false, "app-01")};

  boost::filesystem::remove_all(app_store_.appsDir() / app02.name);
  boost::filesystem::remove_all(app_store_.appsDir() / app03.name);
  setAppsShortlist("app-01, app-02");

  const auto available_targets{check()};
  ASSERT_EQ(1, available_targets.size());
  ASSERT_EQ(target.Name(), available_targets.back().Name());
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(target.Name(), getCurrent().Name());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, OfflineClientOstreeOnly) {
  const auto target{addTarget({createApp("app-01")})};
  // Remove all Target Apps from App store to make sure that only ostree can be updated
  boost::filesystem::remove_all(app_store_.appsDir());
  cfg_.pacman.type = RootfsTreeManager::Name;

  const auto available_targets{check()};
  ASSERT_EQ(1, available_targets.size());
  ASSERT_EQ(target, available_targets.back());
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(target, getCurrent());
}

TEST_F(AkliteOffline, OfflineClientAppsOnly) {
  const auto target{addTarget({createApp("app-01")}, true)};
  const auto available_targets{check()};
  ASSERT_EQ(1, available_targets.size());
  ASSERT_EQ(target, available_targets.back());
  ASSERT_EQ(aklite::cli::StatusCode::InstallAppsNeedFinalization, install());
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(target, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, UpdateIfBootFwUpdateIsNotConfirmedBefore) {
  const auto target{addTarget({createApp("app-01")})};
  // Emulate the situation when the previous ostree update that included boot fw update
  // hasn't been fully completed.
  // I.E. the final reboot that confirms successful reboot on a new boot fw
  // and ostree for the bootloader, so it finalizes the boot fw update and resets `bootupgrade_available`.
  // Also, it may happen that `bootupgrade_available` is set by mistake.
  // The bootloader will detect such situation and reset `bootupgrade_available`.
  boot_flag_mgr_->set("bootupgrade_available");

  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsRebootForBootFw, install());
  reboot();
  boot_flag_mgr_->set("bootupgrade_available", "0");
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(target, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, BootFwUpdate) {
  const auto target{addTarget({createApp("app-01")}, false, true)};

  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::OkNeedsRebootForBootFw, run());
  reboot();
  // emulate boot firmware update confirmation
  boot_flag_mgr_->set("bootupgrade_available", "0");
  ASSERT_EQ(aklite::cli::StatusCode::NoPendingInstallation, run());
  ASSERT_EQ(target, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, UpdateAfterPreloadingWithShortlisting) {
  // emulate preloading with one of the initial Target apps (app01)
  const auto app02{createApp("app-02")};
  preloadApps({createApp("app-01"), app02}, {app02.name});

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto app02_updated{createApp("app-02")};
  const auto new_target{addTarget({createApp("app-01"), app02_updated})};
  // remove app-02 from the install source dir
  boost::filesystem::remove_all(app_store_.appsDir() / app02_updated.name);
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(new_target, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, Rollback) {
  preloadApps({createApp("app-01")}, {});

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01")})};
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  // emulate "normal" rollback - boot on the previous target
  sys_repo_.deploy(initial_target_.Sha256Hash());
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackOk, run());
  ASSERT_EQ(initial_target_, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, RollbackWithAppShortlisting) {
  // emulate preloading with one of the initial Target apps (app01)
  const auto app02{createApp("app-02")};
  preloadApps({createApp("app-01"), app02}, {app02.name});

  // remove the current target apps from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto app02_updated{createApp("app-02")};
  const auto new_target{addTarget({createApp("app-01"), app02_updated, createApp("app-03")})};
  // remove app-02 from the install source dir
  boost::filesystem::remove_all(app_store_.appsDir() / app02_updated.name);
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  // emulate "normal" rollback - boot on the previous target
  sys_repo_.deploy(initial_target_.Sha256Hash());
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackOk, run());
  ASSERT_EQ(initial_target_, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, RollbackIfAppStartFailsWithAppShortlisting) {
  // emulate preloading with one of the initial Target apps (app01)
  const auto app02{createApp("app-02")};
  preloadApps({createApp("app-01"), app02}, {app02.name});

  // remove the current target apps from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto app02_updated{createApp("app-02")};
  const auto new_target{addTarget({createApp("app-01"), app02_updated, createApp("app-03", "compose-start-failure")})};
  // remove app-02 from the install source dir
  boost::filesystem::remove_all(app_store_.appsDir() / app02_updated.name);
  setAppsShortlist("app-01,app-03");
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackNeedsReboot, run());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(initial_target_, getCurrent());
  ASSERT_TRUE(areAppsInSync());
}

TEST_F(AkliteOffline, RollbackToInitialTarget) {
  preloadApps({createApp("app-01")}, {}, false);
  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01")})};
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  // emulate "normal" rollback - boot on the previous target
  sys_repo_.deploy(initial_target_.Sha256Hash());
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackOk, run());
  // If if's so called "initial" target then we don't know what apps were installed
  // at a device initial startup, so we just check if name and ostree hash of targets match.
  ASSERT_EQ(initial_target_.Name(), getCurrent().Name());
  ASSERT_EQ(initial_target_.Sha256Hash(), getCurrent().Sha256Hash());
  ASSERT_TRUE(Target::isInitial(Target::fromTufTarget(getCurrent())));
}

TEST_F(AkliteOffline, RollbackToInitialTargetIfAppDrivenRolllback) {
  const auto app01{createApp("app-01")};
  preloadApps({app01}, {}, false);

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01", "compose-start-failure")})};
  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackNeedsReboot, run());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::Ok, run());
  ASSERT_EQ(initial_target_.Name(), getCurrent().Name());
  ASSERT_EQ(initial_target_.Sha256Hash(), getCurrent().Sha256Hash());
  ASSERT_TRUE(Target::isInitial(Target::fromTufTarget(getCurrent())));
}

TEST_F(AkliteOffline, RollbackToUnknown) {
  // make the initial Target "unknown"
  cfg_.pacman.extra["x-fio-test-no-init-target"] = true;
  const auto app01{createApp("app-01")};
  preloadApps({app01}, {}, false);
  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01")})};

  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  // emulate "normal" rollback - boot on the previous target, which is "unknown"
  sys_repo_.deploy(initial_target_.Sha256Hash());
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackOk, run());
  ASSERT_EQ(initial_target_.Sha256Hash(), getCurrent().Sha256Hash());
  ASSERT_FALSE(Target::isInitial(Target::fromTufTarget(getCurrent())));
}

TEST_F(AkliteOffline, RollbackToUnknownIfAppDrivenRolllback) {
  // make the initial Target "unknown"
  cfg_.pacman.extra["x-fio-test-no-init-target"] = true;
  const auto app01{createApp("app-01")};
  preloadApps({app01}, {}, false);

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01", "compose-start-failure")})};

  ASSERT_EQ(aklite::cli::StatusCode::InstallNeedsReboot, install());
  reboot();
  ASSERT_EQ(aklite::cli::StatusCode::InstallRollbackFailed, run());
  // Cannot perform rollback to unknown, so still booted on a new target's hash
  ASSERT_EQ(new_target.Sha256Hash(), getCurrent().Sha256Hash());
  ASSERT_FALSE(Target::isInitial(Target::fromTufTarget(getCurrent())));
}

TEST_F(AkliteOffline, InvalidTargetInstallOstree) {
  const auto target_not_available{addTarget({createApp("app-01")})};
  const auto target_available{addTarget({createApp("app-01")})};
  // Remove the first target ostree commit so only one target is available for offline update
  // while the TUF repo contains both targets.
  ostree_repo_.removeCommitObject(target_not_available.Sha256Hash());

  AkliteClient client(createLiteClient());
  const auto cr = client.CheckInLocal(src());
  ASSERT_TRUE(cr);
  const auto& available_targtets = cr.Targets();
  ASSERT_EQ(1, available_targtets.size());
  auto ic = client.Installer(target_not_available, "", "", InstallMode::OstreeOnly, src());
  ASSERT_TRUE(ic == nullptr);
}

TEST_F(AkliteOffline, InvalidTargetInstallApps) {
  const auto target_not_available{addTarget({createApp("app-01")})};
  // Remove app-01 from the install source dir so only one target is available for offline update
  // while the TUF repo contains both targets.
  boost::filesystem::remove_all(app_store_.dir());
  const auto target_available{addTarget({createApp("app-02")})};

  AkliteClient client(createLiteClient());
  const auto cr = client.CheckInLocal(src());
  ASSERT_TRUE(cr);
  const auto& available_targtets = cr.Targets();
  ASSERT_EQ(1, available_targtets.size());
  auto ic = client.Installer(target_not_available, "", "", InstallMode::OstreeOnly, src());
  ASSERT_TRUE(ic == nullptr);
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  SysRootFS::CreateCmd = argv[1];
  return RUN_ALL_TESTS();
}
