#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>
#include <fstream>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "appengine.h"
#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"
#include "offline/client.h"
#include "target.h"

// include test fixtures
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/liteclient/boot_flag_mgr.cc"
#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"
#include "fixtures/liteclient/sysostreerepomock.cc"
#include "fixtures/liteclient/sysrootfs.cc"
#include "fixtures/liteclient/tufrepomock.cc"

// Defined in fstatvfs-mock.cc
extern void SetFreeBlockNumb(uint64_t, uint64_t);
extern void UnsetFreeBlockNumb();

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
        boot_flag_mgr_{std::make_shared<FioVb>((test_dir_.Path() / "fiovb").string())},
        initial_target_{Uptane::Target::Unknown()} {
    // hardware ID
    cfg_.provision.primary_ecu_hardware_id = hw_id;
    cfg_.provision.primary_ecu_serial = "test_primary_ecu_serial_id";

    // a path to the config dir
    cfg_.storage.path = test_dir_.Path() / "sota-dir";

    // ostree-based sysroot config
    cfg_.pacman.sysroot = sys_repo_.getPath();
    cfg_.pacman.os = os;
    cfg_.pacman.booted = BootedType::kStaged;
    // In most cases an offline device is not registered and does not have configuration set.
    // If the package manager type is not set in a device config then it is initialized to `ostree` by default.
    // The default value (`ostree`) is not  appropriate for the offline update so it sets the package manager to
    // `ComposeAppManager::Name` by default unless no docker binaries are found on the system.
    // Since the CI/test container doesn't have the docker binaries in its filesystem then we need to enforce
    // the compose app package managaer usage because majority of the tests assume/require it.
    // Also, enforcing of the package manager type can be useful of a system with docker but a user still would
    // like to do only ostree update instead of ostree + apps update.
    cfg_.pacman.extra["enforce_pacman_type"] = ComposeAppManager::Name;

    // configure bootloader/booting related functionality
    cfg_.bootloader.reboot_command = "/bin/true";
    cfg_.bootloader.reboot_sentinel_dir = test_dir_.Path();
    cfg_.bootloader.rollback_mode = RollbackMode::kFioVB;

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
  }

  void SetUp() {
    auto env{boost::this_process::environment()};
    env.set("DOCKER_HOST", daemon_.getUrl());
    SetFreeBlockNumb(90, 100);
  }

  void TearDown() { UnsetFreeBlockNumb(); }

  std::vector<Uptane::Target> check() { return offline::client::check(cfg_, src()); }
  offline::PostInstallAction install() { return offline::client::install(cfg_, src(), daemon_.getClient()); }

  offline::PostRunAction run() { return offline::client::run(cfg_, daemon_.getClient()); }

  const Uptane::Target getCurrent() { return offline::client::getCurrent(cfg_, daemon_.getClient()); }

  void setInitialTarget(const std::string& hash, bool known = true) {
    Uptane::EcuMap ecus{{Uptane::EcuSerial("test_primary_ecu_serial_id"), Uptane::HardwareIdentifier(hw_id)}};
    std::vector<Hash> hashes{Hash(Hash::Type::kSha256, hash)};
    initial_target_ = Uptane::Target{known ? hw_id + "-lmp-1" : Target::InitialTarget, ecus, hashes, 0, "", "OSTREE"};
    // update the initial Target to add the hardware ID so Target::MatchTarget() works correctly
    auto custom{initial_target_.custom_data()};
    custom["hardwareIds"][0] = cfg_.provision.primary_ecu_hardware_id;
    custom["version"] = "1";
    initial_target_.updateCustom(custom);
    // set initial bootloader version
    const auto boot_fw_ver{bootloader::BootloaderLite::getVersion(sys_repo_.getDeploymentPath(), hash)};
    boot_flag_mgr_->set("bootfirmware_version", boot_fw_ver);
  }
  Uptane::Target addTarget(const std::vector<AppEngine::App>& apps, bool just_apps = false,
                           bool add_bootloader_update = false) {
    const auto& latest_target{tuf_repo_.getLatest()};
    std::string version;
    if (version.size() == 0) {
      try {
        version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
      } catch (...) {
        LOG_INFO << "No target available, preparing the first version";
        version = "2";
      }
    }
    auto hash{latest_target.IsValid() ? latest_target.sha256Hash() : initial_target_.sha256Hash()};
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
    const std::string name = hw_id + "-" + os + "-" + version;
    return tuf_repo_.addTarget(name, hash, hw_id, version, apps_json);
  }

  void preloadApps(const std::vector<AppEngine::App>& apps, const std::vector<std::string>& apps_not_to_preload,
                   bool add_installed_versions = true) {
    // do App preloading by installing Target that refers to the current ostree hash
    // and contains the list of apps to preload.
    Uptane::Target preloaded_target{initial_target_};
    Json::Value apps_json;

    std::set<std::string> apps_to_shortlist;
    for (const auto& app : apps) {
      apps_json[app.name]["uri"] = app.uri;
      apps_to_shortlist.emplace(app.name);
    }
    tuf_repo_.addTarget(cfg_.provision.primary_ecu_hardware_id + "-lmp-1", initial_target_.sha256Hash(),
                        cfg_.provision.primary_ecu_hardware_id, "0", apps_json);

    // content-based shortlisting
    for (const auto& app : apps_not_to_preload) {
      boost::filesystem::remove_all(app_store_.appsDir() / app);
      apps_to_shortlist.erase(app);
    }
    setAppsShortlist(boost::algorithm::join(apps_to_shortlist, ","));
    ASSERT_EQ(install(), offline::PostInstallAction::Ok);
    ASSERT_EQ(run(), offline::PostRunAction::Ok);

    if (add_installed_versions) {
      Json::Value installed_target_json;
      installed_target_json[initial_target_.filename()]["hashes"]["sha256"] = initial_target_.sha256Hash();
      installed_target_json[initial_target_.filename()]["length"] = 0;
      installed_target_json[initial_target_.filename()]["is_current"] = true;
      Json::Value custom;
      custom[Target::ComposeAppField] = apps_json;
      custom["name"] = cfg_.provision.primary_ecu_hardware_id + "-lmp";
      custom["version"] = "1";
      custom["hardwareIds"][0] = cfg_.provision.primary_ecu_hardware_id;
      custom["targetFormat"] = "OSTREE";
      custom["arch"] = "arm64";
      installed_target_json[initial_target_.filename()]["custom"] = custom;
      Utils::writeFile(cfg_.import.base_path / "installed_versions", installed_target_json);
      ASSERT_EQ(getCurrent().filename(), initial_target_.filename());
    } else {
      // we need to remove the initial target from the TUF repo so it's not listed in the source TUF repo
      // for the following offline update and as result it will be "unknown" to the client.
      tuf_repo_.reset();
      // Turn the "known" target to the "initial" since it's not in DB/TUF meta
      setInitialTarget(initial_target_.sha256Hash(), false);
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
    Json::Value images;
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

  offline::UpdateSrc src() const {
    return {
        tuf_repo_.getRepoPath(), ostree_repo_.getPath(), app_store_.dir(),
        "" /* target is not specified explicitly and has to be determined based on update content and TUF targets */};
  }

  void setAppsShortlist(const std::string& shortlist) { cfg_.pacman.extra["compose_apps"] = shortlist; }

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
  Uptane::Target initial_target_;
};

const std::string AkliteOffline::hw_id{"raspberrypi4-64"};
const std::string AkliteOffline::os{"lmp"};
const std::string AkliteOffline::branch{hw_id + "-" + os};

TEST_F(AkliteOffline, OfflineClient) {
  const auto prev_target{addTarget({createApp("app-01")})};
  const auto target{addTarget({createApp("app-01")})};

  ASSERT_EQ(2, check().size());
  ASSERT_TRUE(target.MatchTarget(check().front()));
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, OfflineClientInstallNotLatest) {
  const auto target{addTarget({createApp("app-01")})};
  const auto app01_updated{createApp("app-01")};
  const auto latest_target{addTarget({app01_updated})};
  const auto app01_updated_uri{Docker::Uri::parseUri(app01_updated.uri)};
  boost::filesystem::remove_all(app_store_.appsDir() / app01_updated.name / app01_updated_uri.digest.hash());

  ASSERT_EQ(1, check().size());
  ASSERT_TRUE(target.MatchTarget(check().front()));
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, OfflineClientMultipleTargets) {
  const std::vector<Uptane::Target> targets{addTarget({createApp("app-01")}),
                                            addTarget({createApp("app-01"), createApp("app-02")}),
                                            addTarget({createApp("app-02"), createApp("app-03")})};

  const auto found_targets{check()};
  ASSERT_EQ(targets.size(), found_targets.size());
  for (int ii = 0; ii < targets.size(); ++ii) {
    ASSERT_TRUE(targets[ii].MatchTarget(found_targets[found_targets.size() - ii - 1]));
  }
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(targets[targets.size() - 1].MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, OfflineClientShortlistedApps) {
  const auto app03{createApp("zz00-app-03")};
  const auto target{addTarget({createApp("app-01"), createApp("app-02"), app03})};
  // remove zz00-app-03 (app03) from the file system to make sure that offline update succeeds
  // if one of the targets apps is missing in the provided update content and the corresponding app shortlist
  // is set in the client config.
  boost::filesystem::remove_all(app_store_.appsDir() / app03.name);
  setAppsShortlist("app-01, app-02");

  ASSERT_EQ(1, check().size());
  ASSERT_TRUE(target.MatchTarget(check().front()));
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, OfflineClientOstreeOnly) {
  const auto target{addTarget({createApp("app-01")})};
  // Remove all Target Apps from App store to make sure that only ostree can be updated
  boost::filesystem::remove_all(app_store_.appsDir());
  cfg_.pacman.extra["enforce_pacman_type"] = RootfsTreeManager::Name;

  ASSERT_EQ(1, check().size());
  ASSERT_TRUE(target.MatchTarget(check().front()));
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, OfflineClientAppsOnly) {
  const auto target{addTarget({createApp("app-01")}, true)};
  ASSERT_EQ(1, check().size());
  ASSERT_TRUE(target.MatchTarget(check().front()));
  ASSERT_EQ(install(), offline::PostInstallAction::Ok);
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, UpdateIfBootFwUpdateIsNotConfirmedBefore) {
  const auto target{addTarget({createApp("app-01")})};
  offline::PostInstallAction post_install_action{offline::PostInstallAction::Undefined};
  // Emulate the situation when the previous ostree update that included boot fw update
  // hasn't been fully completed.
  // I.E. the final reboot that confirms successful reboot on a new boot fw
  // and ostree for the bootloader, so it finalizes the boot fw update and resets `bootupgrade_available`.
  // Also, it may happen that `bootupgrade_available` is set by mistake.
  // The bootloader will detect such situation and reset `bootupgrade_available`.
  boot_flag_mgr_->set("bootupgrade_available");

  ASSERT_EQ(install(), offline::PostInstallAction::NeedRebootForBootFw);
  reboot();
  boot_flag_mgr_->set("bootupgrade_available", "0");
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, BootFwUpdate) {
  const auto target{addTarget({createApp("app-01")}, false, true)};

  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::OkNeedReboot);
  reboot();
  // emulate boot firmware update confirmation
  boot_flag_mgr_->set("bootupgrade_available", "0");
  ASSERT_EQ(run(), offline::PostRunAction::OkNoPendingInstall);
  ASSERT_TRUE(target.MatchTarget(getCurrent()));
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
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(new_target.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, Rollback) {
  preloadApps({createApp("app-01")}, {});

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01")})};
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  // emulate "normal" rollback - boot on the previous target
  sys_repo_.deploy(initial_target_.sha256Hash());
  ASSERT_EQ(run(), offline::PostRunAction::RollbackOk);
  ASSERT_TRUE(initial_target_.MatchTarget(getCurrent()));
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
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  // emulate "normal" rollback - boot on the previous target
  sys_repo_.deploy(initial_target_.sha256Hash());
  ASSERT_EQ(run(), offline::PostRunAction::RollbackOk);
  ASSERT_TRUE(initial_target_.MatchTarget(getCurrent()));
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
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::RollbackNeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::Ok);
  ASSERT_TRUE(initial_target_.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, RollbackToInitialTarget) {
  preloadApps({createApp("app-01")}, {}, false);
  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01")})};
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  // emulate "normal" rollback - boot on the previous target
  sys_repo_.deploy(initial_target_.sha256Hash());
  ASSERT_EQ(run(), offline::PostRunAction::RollbackOk);
  ASSERT_TRUE(initial_target_.MatchTarget(getCurrent()));
}

TEST_F(AkliteOffline, RollbackToInitialTargetIfAppDrivenRolllback) {
  const auto app01{createApp("app-01")};
  preloadApps({app01}, {}, false);

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01", "compose-start-failure")})};
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::RollbackNeedReboot);
  reboot();
  const auto current{getCurrent()};
  ASSERT_TRUE(initial_target_.MatchTarget(getCurrent()));
  ASSERT_EQ(Target::appsJson(current).size(), 1);
  ASSERT_TRUE(Target::appsJson(current).isMember(app01.name));
  ASSERT_EQ(Target::appsJson(current)[app01.name]["uri"], app01.uri);
}

TEST_F(AkliteOffline, RollbackToUnknown) {
  const auto app01{createApp("app-01")};
  preloadApps({app01}, {}, false);
  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01")})};

  // make the initial Target setting fail, so the current Target is "unknown"
  const Docker::Uri uri{Docker::Uri::parseUri(app01.uri)};
  const auto app_dir{test_dir_.Path() / "reset-apps" / "apps" / uri.app / uri.digest.hash()};
  Utils::writeFile(app_dir / Docker::Manifest::Filename, std::string("broken json"));

  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  // emulate "normal" rollback - boot on the previous target, which is "unknown"
  sys_repo_.deploy(initial_target_.sha256Hash());
  ASSERT_EQ(run(), offline::PostRunAction::RollbackToUnknown);
  const auto current{getCurrent()};
  ASSERT_TRUE(Target::isUnknown(current));
  ASSERT_EQ(current.sha256Hash(), initial_target_.sha256Hash());
}

TEST_F(AkliteOffline, RollbackToUnknownIfAppDrivenRolllback) {
  const auto app01{createApp("app-01")};
  preloadApps({app01}, {}, false);

  // remove the current target app from the store/install source dir
  boost::filesystem::remove_all(app_store_.appsDir());
  const auto new_target{addTarget({createApp("app-01", "compose-start-failure")})};

  // make the initial Target setting fail, so the current Target is "unknown"
  const Docker::Uri uri{Docker::Uri::parseUri(app01.uri)};
  const auto app_dir{test_dir_.Path() / "reset-apps" / "apps" / uri.app / uri.digest.hash()};
  Utils::writeFile(app_dir / Docker::Manifest::Filename, std::string("broken json"));
  ASSERT_EQ(install(), offline::PostInstallAction::NeedReboot);
  reboot();
  ASSERT_EQ(run(), offline::PostRunAction::RollbackToUnknownIfAppFailed);
  // Cannot perform rollback to unknown, so still booted on a new target's hash
  ASSERT_EQ(getCurrent().sha256Hash(), new_target.sha256Hash());
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
