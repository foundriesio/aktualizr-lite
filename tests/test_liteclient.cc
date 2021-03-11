#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "uptane_generator/image_repo.h"
#include "utilities/utils.h"

#include "liteclient.h"
#include "composeappmanager.h"

#include <string>
#include <iostream>

#include "ostree/repo.h"
#include "helpers.h"


using ::testing::NiceMock;
using ::testing::Return;

static std::string run_cmd(const std::string &executable_to_run,
                           const std::vector<std::string> &executable_args,
                           const std::string& cmd_desc) {
  auto res = Process::spawn(executable_to_run, executable_args);
  if (std::get<0>(res) != 0) {
    throw std::runtime_error("Failed to " + cmd_desc + ": " + std::get<2>(res));
  }

  auto std_out = std::get<1>(res);
  boost::trim_right_if(std_out, boost::is_any_of(" \t\r\n"));
  return std_out;
}

class MockAppEngine : public AppEngine {
 public:
  MockAppEngine(bool default_behaviour = true) {
    if (default_behaviour) {
      ON_CALL(*this, fetch).WillByDefault(Return(true));
      ON_CALL(*this, install).WillByDefault(Return(true));
      ON_CALL(*this, run).WillByDefault(Return(true));
      ON_CALL(*this, isRunning).WillByDefault(Return(true));
    }
  }
 public:
  MOCK_METHOD(bool, fetch, (const App& app), (override));
  MOCK_METHOD(bool, install, (const App& app), (override));
  MOCK_METHOD(bool, run, (const App& app), (override));
  MOCK_METHOD(void, remove, (const App& app), (override));
  MOCK_METHOD(bool, isRunning, (const App& app), (const, override));
};

class SysRootFS {
 public:
  static std::string GeneratorPath;
 public:
  SysRootFS(std::string path_in, std::string branch_in, std::string hw_id_in, std::string os_in):
      path{std::move(path_in)}, branch{std::move(branch_in)}, hw_id{std::move(hw_id_in)}, os{std::move(os_in)} {
    run_cmd(GeneratorPath, { path, branch, hw_id, os }, "generate a system rootfs template");
  }

  const std::string path;
  const std::string branch;
  const std::string hw_id;
  const std::string os;
};

std::string SysRootFS::GeneratorPath;

class OSTreeMock {
 public:
  OSTreeMock(std::string repo_path, bool create = false, std::string mode = "archive"): path_{std::move(repo_path)} {

    if (create) {
      run_cmd("ostree", {"init", "--repo", path_, "--mode=" + mode}, "init an ostree repo at " + path_);
      LOG_INFO << "OSTree repo was created at " + path_;
    }
  }

  std::string commit(const std::string& src_dir, const std::string& branch) {
    return run_cmd("ostree", { "commit", "--repo", path_, "--branch", branch, "--tree=dir=" + src_dir },
                   "commit from " + src_dir + " to " + path_);
  }

  void set_mode(const std::string& mode) {
    run_cmd("ostree", {"config", "--repo",  path_, "set", "core.mode", mode}, "set mode for repo " + path_);
  }

 private:
  const std::string path_;
};

class SysOSTreeMock {
 public:
  SysOSTreeMock(std::string sysroot_path,
                std::string os_name):
                  path_{sysroot_path},
                  os_{os_name},
                  repo_{path_ + "/ostree/repo"} {

    boost::filesystem::create_directories(sysroot_path);

    run_cmd("ostree", {"admin", "init-fs", path_}, "init a system rootfs at " + path_);
    run_cmd("ostree", {"admin", "--sysroot=" + sysroot_path, "os-init", os_name},
            "init OS in a system rootfs at " + path_);

    repo_.set_mode("bare-user-only");
    LOG_INFO << "System ostree-based repo has been initialized at " << path_;
  }

  const std::string& path() const { return path_; }
  OSTreeMock& repo() { return repo_; }


  void deploy(const std::string& commit_hash) {
    run_cmd("ostree", {"admin", "--sysroot=" + path_, "deploy", "--os=" + os_, commit_hash}, "deploy " + commit_hash);
  }

 private:
  const std::string path_;
  const std::string os_;
  OSTreeMock repo_;
};


class TreehubMock {
 public:
  static std::string ServerPath;
 public:
  TreehubMock(const std::string& repo_path):
    repo_{repo_path, true},
    port_{TestUtils::getFreePort()},
    process_{ServerPath, port_, repo_path} {
    LOG_INFO << "Treehub is running on port " << port_;
  }
  ~TreehubMock() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

  OSTreeMock& repo() { return repo_; }
  std::string url() { return "http://localhost:" + port_ + "/treehub"; }
  const std::string port() const { return port_; }

 private:
  OSTreeMock repo_;
  std::string port_;
  boost::process::child process_;
};

std::string TreehubMock::ServerPath;

class TufRepoMock {
 public:
  static std::string ServerPath;
 public:
  TufRepoMock(const boost::filesystem::path& root_dir, std::string expires = "",
              std::string correlation_id = "corellatio-id")
      : repo_{root_dir, expires, correlation_id},
        port_{TestUtils::getFreePort()},
        url_{"http://localhost:" + port_},
        process_{ServerPath, port_, "-m", root_dir},
        latest_{Uptane::Target::Unknown()} {
    repo_.generateRepo(KeyType::kED25519);
    TestUtils::waitForServer(url_ + "/");
  }
  ~TufRepoMock() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

 public:
  const std::string& url() { return url_; }
  const Uptane::Target& latest() const { return latest_; }

  Uptane::Target add_target(const std::string& target_name, const std::string& hash,
                            const std::string& hardware_id, const std::string& target_version,
                            const Json::Value& apps_json = Json::Value()) {
    Delegation empty_delegetion{};
    Hash hash_obj{Hash::Type::kSha256, hash};
    Json::Value custom_json;
    custom_json["targetFormat"] = "OSTREE";
    custom_json["version"] = target_version;
    custom_json["docker_compose_apps"] = apps_json;

    repo_.addCustomImage(target_name, hash_obj, 0, hardware_id, "", empty_delegetion, custom_json);

    Json::Value target;
    target["length"] = 0;
    target["hashes"]["sha256"] = hash;
    target["custom"] = custom_json;

    latest_ = Uptane::Target(target_name, target);
    return latest_;
  }

 private:
  ImageRepo repo_;
  std::string port_;
  std::string url_;
  boost::process::child process_;
  Uptane::Target latest_;
};

std::string TufRepoMock::ServerPath;


class LiteClientTest : public ::testing::Test {
 public:
  static std::string SysRootSrc;

 protected:
  LiteClientTest(): sysrootfs_{(test_dir_.Path() / "sysroot-fs").string(), branch, hw_id, os},
                    sysrepo_{(test_dir_.Path() / "sysrepo").string(), os},
                    tuf_repo_{test_dir_.Path() / "repo"},
                    treehub_{(test_dir_.Path() / "treehub").string()},
                    initial_target_{Uptane::Target::Unknown()} {

    auto initial_sysroot_commit_hash = sysrepo_.repo().commit(sysrootfs_.path, sysrootfs_.branch);
    sysrepo_.deploy(initial_sysroot_commit_hash);

    Json::Value target_json;
    target_json["hashes"]["sha256"] = initial_sysroot_commit_hash;
    target_json["custom"]["targetFormat"] = "OSTREE";
    target_json["length"] = 0;
    initial_target_ = Uptane::Target{hw_id + "-" + os + "-1", target_json};
  }

  std::shared_ptr<LiteClient> create_liteclient(bool initial_version = true) {
    Config conf;
    conf.uptane.repo_server = tufRepo().url() + "/repo";
    conf.provision.primary_ecu_hardware_id = hw_id;
    conf.storage.path = test_dir_.Path();

    conf.pacman.type = ComposeAppManager::Name;
    conf.pacman.sysroot = sysrepo_.path();
    conf.pacman.os = os;
    conf.pacman.extra["booted"] = "0";
    conf.pacman.extra["compose_apps_root"] = (test_dir_.Path() / "compose-apps").string();
    conf.pacman.ostree_server = treehub_.url();

    conf.bootloader.reboot_command = "/bin/true";
    conf.bootloader.reboot_sentinel_dir = conf.storage.path;
    conf.import.base_path = test_dir_ / "import";

    if (initial_version) {
      Json::Value ins_ver;
      ins_ver[initial_target_.sha256Hash()] = initial_target_.filename();
      std::string installed_version = Utils::jsonToCanonicalStr(ins_ver);
      Utils::writeFile(conf.import.base_path / "installed_versions", installed_version, true);
      tufRepo().add_target(initial_target_.filename(), initial_target_.sha256Hash(), hw_id, "1");
    }
    app_engine_ = std::make_shared<NiceMock<MockAppEngine>>();
    return std::make_shared<LiteClient>(conf, app_engine_);
  }

  Uptane::Target createNewTarget(const std::vector<AppEngine::App>* apps = nullptr) {
    const auto& latest_target{tufRepo().latest()};
    const std::string version = std::to_string(std::stoi(latest_target.custom_version()) + 1);

    // update rootfs and commit it into Treehub's repo
    const std::string unique_file = Utils::randomUuid();
    const std::string unique_content = Utils::randomUuid();
    Utils::writeFile(sysrootfs().path + "/" + unique_file, unique_content, true);
    auto update_commit_hash = treehub().repo().commit(sysrootfs().path, "lmp");

    Json::Value apps_json;
    if (apps) {
      for (const auto& app: *apps) {
        apps_json[app.name]["uri"] = app.uri;
      }
    }

    // add new target to TUF repo
    const std::string target_name = hw_id + "-" + os + "-" + version;
    return tufRepo().add_target(target_name, update_commit_hash, hw_id, version, apps_json);
  }

  Uptane::Target createNewAppTarget(const std::vector<AppEngine::App>& apps) {
    const auto& latest_target{tufRepo().latest()};

    const std::string version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
    Json::Value apps_json;
    for (const auto& app: apps) {
      apps_json[app.name]["uri"] = app.uri;
    }

    // add new target to TUF repo
    const std::string target_name = hw_id + "-" + os + "-" + version;
    return tufRepo().add_target(target_name, latest_target.sha256Hash(), hw_id, version, apps_json);
  }

  AppEngine::App createNewApp(const std::string& app_name, const std::string& factory = "test-factory") {
    const std::string app_uri = "localhost:" + treehub().port() + "/" + factory + "/" + app_name +
                                "@sha256:7ca42b1567ca068dfd6a5392432a5a36700a4aa3e321922e91d974f832a2f243";
    return {app_name, app_uri};
  }

  void update(LiteClient& client, const Uptane::Target& from_target, const Uptane::Target& to_target) {
    // TODO: remove it once aklite is moved to the newer version of LiteClient that exposes update() method
    ASSERT_TRUE(client.checkForUpdates());
    // TODO: call client->getTarget() once the method is moved to LiteClient
    ASSERT_EQ(client.download(to_target, ""), data::ResultCode::Numeric::kOk);
    ASSERT_EQ(client.install(to_target), data::ResultCode::Numeric::kNeedCompletion);
    // make sure that the new Target hasn't been applied/finalized before reboot
    ASSERT_EQ(client.getCurrent().sha256Hash(), from_target.sha256Hash());
    ASSERT_EQ(client.getCurrent().filename(), from_target.filename());
  }

  void update_apps(LiteClient& client, const Uptane::Target& from_target, const Uptane::Target& to_target,
                   data::ResultCode::Numeric expected_download_code = data::ResultCode::Numeric::kOk,
                   data::ResultCode::Numeric expected_install_code = data::ResultCode::Numeric::kOk) {
    // TODO: remove it once aklite is moved to the newer version of LiteClient that exposes update() method
    ASSERT_TRUE(client.checkForUpdates());
    // TODO: call client->getTarget() once the method is moved to LiteClient
    ASSERT_EQ(client.download(to_target, ""), expected_download_code);

    if (expected_download_code == data::ResultCode::Numeric::kOk) {
      ASSERT_EQ(client.install(to_target), expected_install_code);

      if (expected_install_code == data::ResultCode::Numeric::kOk) {
        // make sure that the new Target has been applied
        ASSERT_EQ(client.getCurrent().sha256Hash(), to_target.sha256Hash());
        ASSERT_EQ(client.getCurrent().filename(), to_target.filename());
      } else {
        ASSERT_EQ(client.getCurrent().sha256Hash(), from_target.sha256Hash());
        ASSERT_EQ(client.getCurrent().filename(), from_target.filename());
      }

    } else {
      ASSERT_EQ(client.getCurrent().sha256Hash(), from_target.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from_target.filename());
    }
  }

  bool areTargetsEqual(const Uptane::Target& lhs, const Uptane::Target& rhs) {
    if ((lhs.sha256Hash() != rhs.sha256Hash()) || (lhs.filename() != rhs.filename())) {
      return false;
    }

    auto lhs_custom = lhs.custom_data().get("docker_compose_apps", Json::nullValue);
    auto rhs_custom = rhs.custom_data().get("docker_compose_apps", Json::nullValue);

    if (lhs_custom == Json::nullValue && rhs_custom == Json::nullValue) {
      return true;
    }

    if ((lhs_custom != Json::nullValue && rhs_custom == Json::nullValue) ||
        (lhs_custom == Json::nullValue && rhs_custom != Json::nullValue)) {
      return false;
    }

    for (Json::ValueConstIterator app_it = lhs_custom.begin(); app_it != lhs_custom.end(); ++app_it) {
      if ((*app_it).isObject() && (*app_it).isMember("uri")) {
        const auto& app_name = app_it.key().asString();
        const auto& app_uri = (*app_it)["uri"].asString();
        if (!rhs_custom.isMember(app_name) || rhs_custom[app_name]["uri"] != app_uri) {
          return false;
        }
      }
    }
    return true;
  }

  void reboot(std::shared_ptr<LiteClient>& client) {
    boost::filesystem::remove(test_dir_.Path() / "need_reboot");
    client = create_liteclient(false);
  }

  void restart(std::shared_ptr<LiteClient>& client) {
    client = create_liteclient(false);
  }

  TufRepoMock& tufRepo() { return tuf_repo_; }
  const Uptane::Target& initial_target() const { return initial_target_; }
  TreehubMock& treehub() { return treehub_; }
  SysRootFS& sysrootfs() { return sysrootfs_; }
  SysOSTreeMock& sysrepo() { return sysrepo_; }
  std::shared_ptr<NiceMock<MockAppEngine>>& appEngine() { return app_engine_; }

 protected:
  static const std::string branch;
  static const std::string hw_id;
  static const std::string os;

 private:
  TemporaryDirectory test_dir_;
  SysRootFS sysrootfs_;
  SysOSTreeMock sysrepo_;
  TufRepoMock tuf_repo_;
  TreehubMock treehub_;
  Uptane::Target initial_target_;
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_;
};

std::string LiteClientTest::SysRootSrc;
const std::string LiteClientTest::branch{"lmp"};
const std::string LiteClientTest::hw_id{"raspberrypi4-64"};
const std::string LiteClientTest::os{"lmp"};


TEST_F(LiteClientTest, OstreeUpdate) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));
  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createNewTarget();
  // update
  update(*client, initial_target(), new_target);
  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target));
}

TEST_F(LiteClientTest, OstreeUpdateRollback) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createNewTarget();

  // update
  update(*client, initial_target(), new_target);

  // deploy the initial version/commit to emulate rollback
  sysrepo().deploy(initial_target().sha256Hash());
  // reboot
  reboot(client);
  // make sure that a rollback has happened and a client is still running the initial Target
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

  // make sure we cannot install the bad version
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*client, known_but_not_installed_versions);
  ASSERT_TRUE(known_local_target(*client, new_target, known_but_not_installed_versions));

  // make sure we can update a device with a new valid Target
  auto new_target_03 = createNewTarget();

  // update
  update(*client, initial_target(), new_target_03);

  // reboot
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target_03));
}

TEST_F(LiteClientTest, OstreeUpdateToLatestAfterManualUpdate) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createNewTarget();

  // update
  update(*client, initial_target(), new_target);

  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target));

  // emulate manuall update to the previous version
  update(*client, new_target, initial_target());

  // reboot device and make sure that the previous version is installed
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

  // make sure we can install the latest version that has been installed before
  // the succesfully installed Target should be "not known"
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*client, known_but_not_installed_versions);
  ASSERT_FALSE(known_local_target(*client, new_target, known_but_not_installed_versions));

  // emulate auto update to the latest
  update(*client, initial_target(), new_target);

  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target));
}

TEST_F(LiteClientTest, AppUpdate) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));
  // Create a new Target that just adds a new an app
  auto new_target = createNewAppTarget({createNewApp("app-01")});
  // update to the latest version
  EXPECT_CALL(*appEngine(), fetch).Times(1);
  // since the Target/app is not installed then no reason to check if the app is running
  EXPECT_CALL(*appEngine(), isRunning).Times(0);
  EXPECT_CALL(*appEngine(), install).Times(0);
  // just call run which includes install if necessary (no ostree update case)
  EXPECT_CALL(*appEngine(), run).Times(1);

  update_apps(*client, initial_target(), new_target);
}

TEST_F(LiteClientTest, OstreeAndAppUpdate) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));
  // Create a new Target: update both rootfs and add new app
  std::vector<AppEngine::App> apps{createNewApp("app-01")};
  auto new_target = createNewTarget(&apps);

  {
    EXPECT_CALL(*appEngine(), fetch).Times(1);
    // since the Target/app is not installed then no reason to check if the app is running
    EXPECT_CALL(*appEngine(), isRunning).Times(0);
    // Just install no need too call run
    EXPECT_CALL(*appEngine(), install).Times(1);
    EXPECT_CALL(*appEngine(), run).Times(0);
    // update to the latest version
    update(*client, initial_target(), new_target);
  }

  {
    // reboot device
    reboot(client);
    ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target));
  }
}

TEST_F(LiteClientTest, AppUpdateDownloadFailure) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));
  // Create a new Target that just adds a new an app
  auto new_target = createNewAppTarget({createNewApp("app-01")});

  ON_CALL(*appEngine(), fetch).WillByDefault(Return(false));

  // update to the latest version
  // fetch retry for three times
  EXPECT_CALL(*appEngine(), fetch).Times(3);
  EXPECT_CALL(*appEngine(), isRunning).Times(0);
  EXPECT_CALL(*appEngine(), install).Times(0);
  EXPECT_CALL(*appEngine(), run).Times(0);

  update_apps(*client, initial_target(), new_target, data::ResultCode::Numeric::kDownloadFailed);
}

TEST_F(LiteClientTest, AppUpdateInstallFailure) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));
  // Create a new Target that just adds a new an app
  auto new_target = createNewAppTarget({createNewApp("app-01")});

  ON_CALL(*appEngine(), run).WillByDefault(Return(false));

  // update to the latest version
  // fetch retry for three times
  EXPECT_CALL(*appEngine(), fetch).Times(1);
  EXPECT_CALL(*appEngine(), isRunning).Times(0);
  EXPECT_CALL(*appEngine(), install).Times(0);
  EXPECT_CALL(*appEngine(), run).Times(1);

  update_apps(*client, initial_target(), new_target, data::ResultCode::Numeric::kOk,
              data::ResultCode::Numeric::kInstallFailed);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  logger_init();

  if (argc != 4) {
    std::cerr << "Error: " << argv[0] << " requires the path to the fake TUF repo server\n";
    return EXIT_FAILURE;
  }

  TufRepoMock::ServerPath = argv[1];
  TreehubMock::ServerPath = argv[2];
  SysRootFS::GeneratorPath = argv[3];

  return RUN_ALL_TESTS();
}
