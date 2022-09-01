#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
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
#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"
#include "fixtures/liteclient/sysostreerepomock.cc"
#include "fixtures/liteclient/sysrootfs.cc"
#include "fixtures/liteclient/tufrepomock.cc"

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
        app_store_{test_dir_.Path() / "apps"} {
    // hardware ID
    cfg_.provision.primary_ecu_hardware_id = hw_id;

    // a path to the config dir
    cfg_.storage.path = test_dir_.Path() / "sota-dir";

    // ostree-based sysroot config
    cfg_.pacman.sysroot = sys_repo_.getPath();
    cfg_.pacman.os = os;
    cfg_.pacman.booted = BootedType::kStaged;
    cfg_.pacman.type = "ostree+compose_apps";

    // configure bootloader/booting related functionality
    cfg_.bootloader.reboot_command = "/bin/true";
    cfg_.bootloader.reboot_sentinel_dir = test_dir_.Path();

    cfg_.pacman.extra["reset_apps"] = "";
    cfg_.pacman.extra["reset_apps_root"] = (test_dir_.Path() / "reset-apps").string();
    cfg_.pacman.extra["compose_apps_root"] = (test_dir_.Path() / "compose-apps").string();
    cfg_.pacman.extra["docker_compose_bin"] =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";
    cfg_.pacman.extra["images_data_root"] = daemon_.dataRoot();

    // add an initial rootfs to the ostree-based sysroot that liteclient manages and deploy it
    const auto hash = sys_repo_.getRepo().commit(sys_rootfs_.path, sys_rootfs_.branch);
    sys_repo_.deploy(hash);
  }

  Uptane::Target addTarget(const std::vector<AppEngine::App>* apps = nullptr) {
    const auto& latest_target{tuf_repo_.getLatest()};
    std::string version;
    if (version.size() == 0) {
      try {
        version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
      } catch (...) {
        LOG_INFO << "No target available, preparing the first version";
        version = "1";
      }
    }

    // update rootfs and commit it into Treehub's repo
    const std::string unique_content = Utils::randomUuid();
    const std::string unique_file = Utils::randomUuid();
    Utils::writeFile(sys_rootfs_.path + "/" + unique_file, unique_content, true);
    auto hash = ostree_repo_.commit(sys_rootfs_.path, branch);

    Json::Value apps_json;
    if (apps) {
      for (const auto& app : *apps) {
        apps_json[app.name]["uri"] = app.uri;
      }
    }

    // add new target to TUF repo
    const std::string name = hw_id + "-" + os + "-" + version;
    return tuf_repo_.addTarget(name, hash, hw_id, version, apps_json);
  }

  const std::string getSentinelFilePath() const {
    return (cfg_.bootloader.reboot_sentinel_dir / "need_reboot").string();
  }

  void reboot() { boost::filesystem::remove(getSentinelFilePath()); }

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
};

const std::string AkliteOffline::hw_id{"raspberrypi4-64"};
const std::string AkliteOffline::os{"lmp"};
const std::string AkliteOffline::branch{hw_id + "-" + os};

TEST_F(AkliteOffline, OfflineClient) {
  Uptane::Target target{Uptane::Target::Unknown()};

  const auto layer_size{1024};
  Json::Value layers;
  layers["layers"][0]["digest"] =
      "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
  layers["layers"][0]["size"] = layer_size;

  const auto app01{app_store_.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers))};
  const auto app02{app_store_.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-02", layers))};
  {
    const std::vector<AppEngine::App> apps{app01};
    addTarget(&apps);
  }
  const std::vector<AppEngine::App> apps{app01, app02};
  target = addTarget(&apps);

  offline::UpdateSrc src{
      tuf_repo_.getRepoPath(), ostree_repo_.getPath(), app_store_.dir(),
      "" /* target is not specified explicitly and has to be determined based on update content and TUF targets */
  };

  offline::PostInstallAction post_install_action{offline::PostInstallAction::Undefined};
  ASSERT_NO_THROW(post_install_action = offline::client::install(cfg_, src));
  ASSERT_EQ(post_install_action, offline::PostInstallAction::NeedReboot);

  reboot();

  ASSERT_NO_THROW(offline::client::run(cfg_, daemon_.getClient()));
}

TEST_F(AkliteOffline, OfflineClientOstreeOnly) {
  Uptane::Target target{Uptane::Target::Unknown()};

  const auto layer_size{1024};
  Json::Value layers;
  layers["layers"][0]["digest"] =
      "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
  layers["layers"][0]["size"] = layer_size;

  const auto app01{app_store_.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers))};
  const auto app02{app_store_.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-02", layers))};
  {
    const std::vector<AppEngine::App> apps{app01};
    addTarget(&apps);
  }
  const std::vector<AppEngine::App> apps{app01, app02};
  target = addTarget(&apps);

  // Remove all Target Apps from App store to make sure that only ostree can be updated
  boost::filesystem::remove_all(app_store_.appsDir());
  offline::UpdateSrc src{
      tuf_repo_.getRepoPath(), ostree_repo_.getPath(), app_store_.dir(),
      "" /* target is not specified explicitly and has to be determined based on update content and TUF targets */
  };

  offline::PostInstallAction post_install_action{offline::PostInstallAction::Undefined};
  ASSERT_NO_THROW(post_install_action = offline::client::install(cfg_, src, daemon_.getClient()));
  ASSERT_EQ(post_install_action, offline::PostInstallAction::NeedReboot);

  reboot();

  ASSERT_NO_THROW(offline::client::run(cfg_, daemon_.getClient()));
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
