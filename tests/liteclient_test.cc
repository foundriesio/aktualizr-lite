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

#include "composeappmanager.h"
#include "liteclient.h"

#include <iostream>
#include <string>

#include "helpers.h"
#include "ostree/repo.h"

using ::testing::NiceMock;
using ::testing::Return;

static std::string executeCmd(const std::string& cmd, const std::vector<std::string>& args, const std::string& desc) {
  auto res = Process::spawn(cmd, args);
  if (std::get<0>(res) != 0) throw std::runtime_error("Failed to " + desc + ": " + std::get<2>(res));

  auto std_out = std::get<1>(res);
  boost::trim_right_if(std_out, boost::is_any_of(" \t\r\n"));
  return std_out;
}

/**
 * Class MockAppEngine
 *
 */
class MockAppEngine : public AppEngine {
 public:
  MockAppEngine(bool default_behaviour = true) {
    if (!default_behaviour) return;

    ON_CALL(*this, fetch).WillByDefault(Return(true));
    ON_CALL(*this, install).WillByDefault(Return(true));
    ON_CALL(*this, run).WillByDefault(Return(true));
    ON_CALL(*this, isRunning).WillByDefault(Return(true));
  }

 public:
  MOCK_METHOD(bool, fetch, (const App& app), (override));
  MOCK_METHOD(bool, install, (const App& app), (override));
  MOCK_METHOD(bool, run, (const App& app), (override));
  MOCK_METHOD(void, remove, (const App& app), (override));
  MOCK_METHOD(bool, isRunning, (const App& app), (const, override));
};

/**
 * Class SysRootFS
 *
 */
class SysRootFS {
 public:
  static std::string CreateCmd;

 public:
  SysRootFS(std::string _path, std::string _branch, std::string _hw_id, std::string _os)
      : branch{std::move(_branch)}, hw_id{std::move(_hw_id)}, path{std::move(_path)}, os{std::move(_os)} {
    executeCmd(CreateCmd, {path, branch, hw_id, os}, "generate a system rootfs template");
  }

  const std::string branch;
  const std::string hw_id;
  const std::string path;
  const std::string os;
};

std::string SysRootFS::CreateCmd;

/**
 * Class OSTreeRepoMock
 *
 */
class OSTreeRepoMock {
 public:
  OSTreeRepoMock(std::string path, bool create = false, std::string mode = "archive") : path_{std::move(path)} {
    if (!create) return;
    executeCmd("ostree", {"init", "--repo", path_, "--mode=" + mode}, "init an ostree repo at " + path_);
    LOG_INFO << "OSTree repo was created at " + path_;
  }

  std::string commit(const std::string& src_dir, const std::string& branch) {
    return executeCmd("ostree", {"commit", "--repo", path_, "--branch", branch, "--tree=dir=" + src_dir},
                      "commit from " + src_dir + " to " + path_);
  }

  void set_mode(const std::string& mode) {
    executeCmd("ostree", {"config", "--repo", path_, "set", "core.mode", mode}, "set mode for repo " + path_);
  }

  const std::string& getPath() const { return path_; }

 private:
  const std::string path_;
};

/**
 * Class SysOSTreeRepoMock
 *
 */
class SysOSTreeRepoMock {
 public:
  SysOSTreeRepoMock(std::string _path, std::string _os) : path_{_path}, os_{_os}, repo_{path_ + "/ostree/repo"} {
    boost::filesystem::create_directories(path_);
    executeCmd("ostree", {"admin", "init-fs", path_}, "init a system rootfs at " + path_);
    executeCmd("ostree", {"admin", "--sysroot=" + path_, "os-init", os_}, "init OS in a system rootfs at " + path_);
    repo_.set_mode("bare-user-only");
    LOG_INFO << "System ostree-based repo has been initialized at " << path_;
  }

  const std::string& getPath() const { return path_; }
  OSTreeRepoMock& getRepo() { return repo_; }

  void deploy(const std::string& hash) {
    executeCmd("ostree", {"admin", "--sysroot=" + path_, "deploy", "--os=" + os_, hash}, "deploy " + hash);
  }

 private:
  const std::string path_;
  const std::string os_;
  OSTreeRepoMock repo_;
};

/**
 * Class TufRepoMock
 *
 */
class TufRepoMock {
 public:
  TufRepoMock(const boost::filesystem::path& _root, std::string expires = "",
              std::string correlation_id = "corellatio-id")
      : root_{_root.string()}, repo_{_root, expires, correlation_id}, latest_{Uptane::Target::Unknown()} {
    repo_.generateRepo(KeyType::kED25519);
  }

 public:
  const std::string& getPath() const { return root_; }
  const Uptane::Target& getLatest() const { return latest_; }

  Uptane::Target addTarget(const std::string& name, const std::string& hash, const std::string& hardware_id,
                           const std::string& version, const Json::Value& apps_json = Json::Value()) {
    Delegation null_delegation{};
    Hash hash_obj{Hash::Type::kSha256, hash};

    Json::Value custom_json;
    custom_json["targetFormat"] = "OSTREE";
    custom_json["version"] = version;
    custom_json["docker_compose_apps"] = apps_json;
    repo_.addCustomImage(name, hash_obj, 0, hardware_id, "", null_delegation, custom_json);

    Json::Value target_json;
    target_json["length"] = 0;
    target_json["hashes"]["sha256"] = hash;
    target_json["custom"] = custom_json;
    latest_ = Uptane::Target(name, target_json);
    return latest_;
  }

 private:
  const std::string root_;
  ImageRepo repo_;
  Uptane::Target latest_;
};

/**
 * Class DeviceGatewayMock
 *
 */
class DeviceGatewayMock {
 public:
  static std::string RunCmd;

 public:
  DeviceGatewayMock(const OSTreeRepoMock& ostree, const TufRepoMock& tuf)
      : ostree_{ostree},
        tuf_{tuf},
        port_{TestUtils::getFreePort()},
        url_{"http://localhost:" + port_},
        req_headers_file_{tuf_.getPath() + "/headers.json"},
        process_{RunCmd,           "--port",         port_, "--ostree", ostree_.getPath(), "--tuf-repo", tuf_.getPath(),
                 "--headers-file", req_headers_file_} {
    TestUtils::waitForServer(url_ + "/");
    LOG_INFO << "Device Gateway is running on port " << port_;
  }

  ~DeviceGatewayMock() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

 public:
  std::string getOsTreeUri() const { return url_ + "/treehub"; }
  std::string getTufRepoUri() const { return url_ + "/repo"; }
  const std::string& getPort() const { return port_; }
  Json::Value getReqHeaders() const { return Utils::parseJSONFile(req_headers_file_); }

 private:
  const OSTreeRepoMock& ostree_;
  const TufRepoMock& tuf_;
  const std::string port_;
  const std::string url_;
  const std::string req_headers_file_;
  boost::process::child process_;
};

std::string DeviceGatewayMock::RunCmd;

/**
 * Class LiteClientTest
 *
 */
class LiteClientTest : public ::testing::Test {
 public:
  static std::string SysRootSrc;

 protected:
  LiteClientTest()
      : sys_rootfs_{(test_dir_.Path() / "sysroot-fs").string(), branch, hw_id, os},
        sys_repo_{(test_dir_.Path() / "sysrepo").string(), os},
        tuf_repo_{test_dir_.Path() / "repo"},
        ostree_repo_{(test_dir_.Path() / "treehub").string(), true},
        device_gateway_{ostree_repo_, tuf_repo_},
        initial_target_{Uptane::Target::Unknown()},
        sysroot_hash_{sys_repo_.getRepo().commit(sys_rootfs_.path, sys_rootfs_.branch)} {
    sys_repo_.deploy(sysroot_hash_);
  }

  enum class InitialVersion { kOff, kOn, kCorrupted1, kCorrupted2 };

  /**
   * method createLiteClient
   */
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none) {
    Config conf;
    conf.uptane.repo_server = device_gateway_.getTufRepoUri();
    conf.provision.primary_ecu_hardware_id = hw_id;
    conf.storage.path = test_dir_.Path();

    conf.pacman.type = ComposeAppManager::Name;
    conf.pacman.sysroot = sys_repo_.getPath();
    conf.pacman.os = os;
    conf.pacman.extra["booted"] = "0";
    conf.pacman.extra["compose_apps_root"] = (test_dir_.Path() / "compose-apps").string();
    if (!!apps) {
      conf.pacman.extra["compose_apps"] = boost::algorithm::join(*apps, ",");
    }
    app_shortlist_ = apps;
    conf.pacman.ostree_server = device_gateway_.getOsTreeUri();

    conf.bootloader.reboot_command = "/bin/true";
    conf.bootloader.reboot_sentinel_dir = conf.storage.path;
    conf.import.base_path = test_dir_ / "import";

    if (initial_version == InitialVersion::kOn || initial_version == InitialVersion::kCorrupted1 ||
        initial_version == InitialVersion::kCorrupted2) {
      /*
       * Sample LMP/OE generated installed_version file
       *
       * {
       *   "raspberrypi4-64-lmp" {
       *      "hashes": {
       *        "sha256": "cbf23f479964f512ff1d0b01a688d096a670d1d099c1ee3d46baea203e7ef4ab"
       *      },
       *      "is_current": true,
       *      "custom": {
       *        "targetFormat": "OSTREE",
       *        "name": "raspberrypi4-64-lmp",
       *        "version": "1",
       *        "hardwareIds": [
       *                       "raspberrypi4-64"
       *                       ],
       *        "lmp-manifest-sha": "0db09a7e9bac87ef2127e5be8d11f23b3e18513c",
       *        "arch": "aarch64",
       *        "image-file": "lmp-factory-image-raspberrypi4-64.wic.gz",
       *        "meta-subscriber-overrides-sha": "43093be20fa232ef5fe17135115bac4327b501bd",
       *        "tags": [
       *                "master"
       *                ],
       *        "docker_compose_apps": {
       *          "app-01": {
       *            "uri":
       * "hub.foundries.io/msul-dev01/app-06@sha256:2e7b8bc87c67f6042fb88e575a1c73bf70d114f3f2fd1a7aeb3d1bf3b6a0737f"
       *          },
       *          "app-02": {
       *            "uri":
       * "hub.foundries.io/msul-dev01/app-05@sha256:267b14e2e0e98d7e966dbd49bddaa792e5d07169eb3cf2462bbbfecac00f46ef"
       *          }
       *        },
       *        "containers-sha": "a041e7a0aa1a8e73a875b4c3fdf9a418d3927894"
       *     }
       *  }
       */

      Json::Value installed_version;
      // corrupted1 will invalidate the sysroot_hash_ sha256
      installed_version["hashes"]["sha256"] =
          sysroot_hash_ + (initial_version == InitialVersion::kCorrupted1 ? "DEADBEEF" : "");
      installed_version["is_current"] = true;
      installed_version["custom"]["name"] = hw_id + "-" + os;
      installed_version["custom"]["version"] = "1";
      installed_version["custom"]["hardwareIds"] = hw_id;
      installed_version["custom"]["targetFormat"] = "OSTREE";
      installed_version["custom"]["arch"] = "aarch64";
      installed_version["custom"]["image-file"] = "lmp-factory-image-raspberrypi4-64.wic.gz";
      installed_version["custom"]["tags"] = "master";

      /* create the initial_target from the above json file: pass the root node
       * name as a parameter
       */
      initial_target_ = Uptane::Target{hw_id + "-" + os + "-" + "1", installed_version};

      Json::Value ins_ver;
      // set the root node name
      ins_ver[initial_target_.filename()] = installed_version;
      // write the json information to a file (corrupted2 will write a corrupted  file)
      Utils::writeFile(conf.import.base_path / "installed_versions",
                       (initial_version == InitialVersion::kCorrupted2) ? "deadbeef\t\ncorrupted file\n\n"
                                                                        : Utils::jsonToCanonicalStr(ins_ver),
                       true);

      getTufRepo().addTarget(initial_target_.filename(), initial_target_.sha256Hash(), hw_id, "1");
    }
    app_engine_ = std::make_shared<NiceMock<MockAppEngine>>();
    return std::make_shared<LiteClient>(conf, app_engine_);
  }

  /**
   * method createTarget
   */
  Uptane::Target createTarget(const std::vector<AppEngine::App>* apps = nullptr) {
    const auto& latest_target{getTufRepo().getLatest()};
    std::string version;
    try {
      version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
    } catch (...) {
      LOG_INFO << "No target available, preparing the first version";
      version = "1";
    }

    // update rootfs and commit it into Treehub's repo
    const std::string unique_content = Utils::randomUuid();
    const std::string unique_file = Utils::randomUuid();
    Utils::writeFile(getSysRootFs().path + "/" + unique_file, unique_content, true);
    auto hash = getOsTreeRepo().commit(getSysRootFs().path, "lmp");

    Json::Value apps_json;
    if (apps) {
      for (const auto& app : *apps) {
        apps_json[app.name]["uri"] = app.uri;
      }
    }

    // add new target to TUF repo
    const std::string name = hw_id + "-" + os + "-" + version;
    return getTufRepo().addTarget(name, hash, hw_id, version, apps_json);
  }

  /**
   * method createAppTarget
   */
  Uptane::Target createAppTarget(const std::vector<AppEngine::App>& apps) {
    const auto& latest{getTufRepo().getLatest()};
    const std::string version = std::to_string(std::stoi(latest.custom_version()) + 1);
    Json::Value apps_json;
    for (const auto& app : apps) {
      apps_json[app.name]["uri"] = app.uri;
    }

    // add new target to TUF repo
    const std::string name = hw_id + "-" + os + "-" + version;
    return getTufRepo().addTarget(name, latest.sha256Hash(), hw_id, version, apps_json);
  }

  /**
   * method createApp
   */
  AppEngine::App createApp(const std::string& name,
                           const std::string& hash = "7ca42b1567ca068dfd6a5392432a5a36700a4aa3e321922e91d974f832a2f243",
                           const std::string& factory = "test-factory") {
    const std::string uri =
        "localhost:" + getDeviceGateway().getPort() + "/" + factory + "/" + name + "@sha256:" + hash;
    return {name, uri};
  }

  /**
   * mehod update
   */
  void update(LiteClient& client, const Uptane::Target& from, const Uptane::Target& to) {
    // TODO: remove it once aklite is moved to the newer version of LiteClient that exposes update() method
    ASSERT_TRUE(client.checkForUpdates());

    // TODO: call client->getTarget() once the method is moved to LiteClient
    ASSERT_EQ(client.download(to, ""), data::ResultCode::Numeric::kOk);
    ASSERT_EQ(client.install(to), data::ResultCode::Numeric::kNeedCompletion);

    // make sure that the new Target hasn't been applied/finalized before reboot
    ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
    ASSERT_EQ(client.getCurrent().filename(), from.filename());
    checkHeaders(client, from);
  }

  /**
   * method updateApps
   */
  void updateApps(LiteClient& client, const Uptane::Target& from, const Uptane::Target& to,
                  data::ResultCode::Numeric expected_download_code = data::ResultCode::Numeric::kOk,
                  data::ResultCode::Numeric expected_install_code = data::ResultCode::Numeric::kOk) {
    // TODO: remove it once aklite is moved to the newer version of LiteClient that exposes update() method
    ASSERT_TRUE(client.checkForUpdates());

    // TODO: call client->getTarget() once the method is moved to LiteClient
    ASSERT_EQ(client.download(to, ""), expected_download_code);

    if (expected_download_code != data::ResultCode::Numeric::kOk) {
      ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from.filename());
      checkHeaders(client, from);
      return;
    }

    ASSERT_EQ(client.install(to), expected_install_code);
    if (expected_install_code == data::ResultCode::Numeric::kOk) {
      // make sure that the new Target has been applied
      ASSERT_EQ(client.getCurrent().sha256Hash(), to.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), to.filename());
      // TODO: the daemon_main is emulated,
      // see
      // https://github.com/foundriesio/aktualizr-lite/blob/7ab6998920d57605601eda16f9bebedf00cc1f7f/src/main.cc#L264
      // once the daemon_main is "cleaned" the updateHeader can be removed from the test.
      LiteClient::update_request_headers(client.http_client, to, client.config.pacman);
      checkHeaders(client, to);
    } else {
      ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from.filename());
      checkHeaders(client, from);
    }
  }

  /**
   * method targetsMatch
   */
  bool targetsMatch(const Uptane::Target& lhs, const Uptane::Target& rhs) {
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
      if (!(*app_it).isObject() || !(*app_it).isMember("uri")) {
        continue;
      }

      const auto& app_name = app_it.key().asString();
      const auto& app_uri = (*app_it)["uri"].asString();
      if (!rhs_custom.isMember(app_name) || rhs_custom[app_name]["uri"] != app_uri) {
        return false;
      }
    }
    return true;
  }

  /**
   * method reboot
   */
  void reboot(std::shared_ptr<LiteClient>& client) {
    boost::filesystem::remove(test_dir_.Path() / "need_reboot");
    client = createLiteClient(InitialVersion::kOff, app_shortlist_);
  }

  /**
   * method restart
   */
  void restart(std::shared_ptr<LiteClient>& client) { client = createLiteClient(InitialVersion::kOff); }

  /**
   * method checkHeaders
   */
  void checkHeaders(LiteClient& client, const Uptane::Target& target) {
    // check for a new Target in order to send requests with headers we are interested in
    ASSERT_TRUE(client.checkForUpdates());
    if (target.MatchTarget(Uptane::Target::Unknown())) return;

    auto req_headers = getDeviceGateway().getReqHeaders();
    ASSERT_EQ(req_headers["x-ats-ostreehash"], target.sha256Hash());
    ASSERT_EQ(req_headers["x-ats-target"], target.filename());

    auto target_apps = target.custom_data()["docker_compose_apps"];
    std::vector<std::string> apps;
    for (Json::ValueIterator ii = target_apps.begin(); ii != target_apps.end(); ++ii) {
      if ((*ii).isObject() && (*ii).isMember("uri")) {
        const auto& target_app_name = ii.key().asString();
        if (!app_shortlist_ ||
            (*app_shortlist_).end() != std::find((*app_shortlist_).begin(), (*app_shortlist_).end(), target_app_name)) {
          apps.push_back(target_app_name);
        }
      }
    }

    std::string apps_list = boost::algorithm::join(apps, ",");
    ASSERT_EQ(req_headers.get("x-ats-dockerapps", ""), apps_list);
  }

  /**
   * methods: miscellaneous
   */
  void setInitialTarget(const Uptane::Target& target) { initial_target_ = target; }
  const Uptane::Target& getInitialTarget() const { return initial_target_; }

  std::shared_ptr<NiceMock<MockAppEngine>>& getAppEngine() { return app_engine_; }
  DeviceGatewayMock& getDeviceGateway() { return device_gateway_; }
  SysOSTreeRepoMock& getSysRepo() { return sys_repo_; }
  SysRootFS& getSysRootFs() { return sys_rootfs_; }
  TufRepoMock& getTufRepo() { return tuf_repo_; }
  OSTreeRepoMock& getOsTreeRepo() { return ostree_repo_; }
  void setAppShortlist(const std::vector<std::string>& apps) { app_shortlist_ = boost::make_optional(apps); }

 protected:
  static const std::string branch;
  static const std::string hw_id;
  static const std::string os;

 private:
  TemporaryDirectory test_dir_;  // must be the first element in the class
  SysRootFS sys_rootfs_;
  SysOSTreeRepoMock sys_repo_;
  TufRepoMock tuf_repo_;
  OSTreeRepoMock ostree_repo_;
  DeviceGatewayMock device_gateway_;
  Uptane::Target initial_target_;
  const std::string sysroot_hash_;
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_;
  boost::optional<std::vector<std::string>> app_shortlist_;
};

std::string LiteClientTest::SysRootSrc;
const std::string LiteClientTest::branch{"lmp"};
const std::string LiteClientTest::hw_id{"raspberrypi4-64"};
const std::string LiteClientTest::os{"lmp"};

/*----------------------------------------------------------------------------*/
/*  TESTS                                                                     */
/*                                                                            */
/*----------------------------------------------------------------------------*/
TEST_F(LiteClientTest, OstreeUpdateWhenNoInstalledVersions) {
  // boot device with no installed versions
  auto client = createLiteClient(InitialVersion::kOff);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  // check there is still no target
  auto req_headers = getDeviceGateway().getReqHeaders();
  ASSERT_EQ(req_headers["x-ats-target"], Uptane::Target::Unknown().filename());
  ASSERT_FALSE(new_target.MatchTarget(Uptane::Target::Unknown()));

  // verify the install
  ASSERT_TRUE(client->getCurrent().MatchTarget(Uptane::Target::Unknown()));
  reboot(client);
  ASSERT_FALSE(new_target.MatchTarget(Uptane::Target::Unknown()));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
}

TEST_F(LiteClientTest, OstreeUpdateInstalledVersionsCorrupted1) {
  // boot device with an invalid initial_version json file (ostree sha)
  auto client = createLiteClient(InitialVersion::kCorrupted1);

  // verify that the initial version was corrupted
  ASSERT_FALSE(targetsMatch(client->getCurrent(), getInitialTarget()));
  setInitialTarget(Uptane::Target::Unknown());

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  auto req_headers = getDeviceGateway().getReqHeaders();
  ASSERT_EQ(req_headers["x-ats-target"], Uptane::Target::Unknown().filename());
  ASSERT_FALSE(new_target.MatchTarget(Uptane::Target::Unknown()));

  // verify the install
  ASSERT_TRUE(client->getCurrent().MatchTarget(Uptane::Target::Unknown()));
  reboot(client);
  ASSERT_FALSE(new_target.MatchTarget(Uptane::Target::Unknown()));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
}

TEST_F(LiteClientTest, OstreeUpdateInstalledVersionsCorrupted2) {
  // boot device with a corrupted json file in the filesystem
  auto client = createLiteClient(InitialVersion::kCorrupted2);

  // verify that the initial version was corrupted
  ASSERT_FALSE(targetsMatch(client->getCurrent(), getInitialTarget()));
  setInitialTarget(Uptane::Target::Unknown());

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  auto req_headers = getDeviceGateway().getReqHeaders();
  ASSERT_EQ(req_headers["x-ats-target"], Uptane::Target::Unknown().filename());
  ASSERT_FALSE(new_target.MatchTarget(Uptane::Target::Unknown()));

  // verify the install
  ASSERT_TRUE(client->getCurrent().MatchTarget(Uptane::Target::Unknown()));
  reboot(client);
  ASSERT_FALSE(new_target.MatchTarget(Uptane::Target::Unknown()));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
}

TEST_F(LiteClientTest, OstreeUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  // reboot device
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
}

TEST_F(LiteClientTest, OstreeUpdateRollback) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  // deploy the initial version/commit to emulate rollback
  getSysRepo().deploy(getInitialTarget().sha256Hash());

  reboot(client);
  // make sure that a rollback has happened and a client is still running the initial Target
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  checkHeaders(*client, getInitialTarget());

  // make sure we cannot install the bad version
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*client, known_but_not_installed_versions);
  ASSERT_TRUE(known_local_target(*client, new_target, known_but_not_installed_versions));

  // make sure we can update a device with a new valid Target
  auto new_target_03 = createTarget();
  update(*client, getInitialTarget(), new_target_03);

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target_03));
  checkHeaders(*client, new_target_03);
}

TEST_F(LiteClientTest, OstreeUpdateToLatestAfterManualUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  // reboot device
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);

  // emulate manuall update to the previous version
  update(*client, new_target, getInitialTarget());

  // reboot device and make sure that the previous version is installed
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  checkHeaders(*client, getInitialTarget());

  // make sure we can install the latest version that has been installed before
  // the succesfully installed Target should be "not known"
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*client, known_but_not_installed_versions);
  ASSERT_FALSE(known_local_target(*client, new_target, known_but_not_installed_versions));

  // emulate auto update to the latest
  update(*client, getInitialTarget(), new_target);

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
}

TEST_F(LiteClientTest, AppUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target that just adds a new an app
  auto new_target = createAppTarget({createApp("app-01")});

  // update to the latest version
  EXPECT_CALL(*getAppEngine(), fetch).Times(1);

  // since the Target/app is not installed then no reason to check if the app is running
  EXPECT_CALL(*getAppEngine(), isRunning).Times(0);
  EXPECT_CALL(*getAppEngine(), install).Times(0);

  // just call run which includes install if necessary (no ostree update case)
  EXPECT_CALL(*getAppEngine(), run).Times(1);

  updateApps(*client, getInitialTarget(), new_target);
}

TEST_F(LiteClientTest, AppUpdateWithShortlist) {
  // boot device
  auto client = createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{"app-02"}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target that adds two new apps
  auto new_target = createAppTarget({createApp("app-01"), createApp("app-02")});

  // update to the latest version
  EXPECT_CALL(*getAppEngine(), fetch).Times(1);
  EXPECT_CALL(*getAppEngine(), isRunning).Times(0);
  EXPECT_CALL(*getAppEngine(), install).Times(0);
  // run should be called once since only one app is specified in the config
  EXPECT_CALL(*getAppEngine(), run).Times(1);

  updateApps(*client, getInitialTarget(), new_target);
}

TEST_F(LiteClientTest, AppUpdateWithEmptyShortlist) {
  // boot device
  auto client = createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{""}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target that adds two new apps
  auto new_target = createAppTarget({createApp("app-01"), createApp("app-02")});

  // update to the latest version, nothing should be called since an empty app list is specified in the config
  EXPECT_CALL(*getAppEngine(), fetch).Times(0);
  EXPECT_CALL(*getAppEngine(), isRunning).Times(0);
  EXPECT_CALL(*getAppEngine(), install).Times(0);
  EXPECT_CALL(*getAppEngine(), run).Times(0);

  updateApps(*client, getInitialTarget(), new_target);
}

TEST_F(LiteClientTest, OstreeAndAppUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  std::vector<AppEngine::App> apps{createApp("app-01")};
  auto new_target = createTarget(&apps);

  {
    EXPECT_CALL(*getAppEngine(), fetch).Times(1);

    // since the Target/app is not installed then no reason to check if the app is running
    EXPECT_CALL(*getAppEngine(), isRunning).Times(0);

    // Just install no need too call run
    EXPECT_CALL(*getAppEngine(), install).Times(1);
    EXPECT_CALL(*getAppEngine(), run).Times(0);

    // update to the latest version
    update(*client, getInitialTarget(), new_target);
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    checkHeaders(*client, new_target);
  }
}

TEST_F(LiteClientTest, AppUpdateDownloadFailure) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target that just adds a new an app
  auto new_target = createAppTarget({createApp("app-01")});

  ON_CALL(*getAppEngine(), fetch).WillByDefault(Return(false));

  // update to the latest version
  // fetch retry for three times
  EXPECT_CALL(*getAppEngine(), fetch).Times(3);
  EXPECT_CALL(*getAppEngine(), isRunning).Times(0);
  EXPECT_CALL(*getAppEngine(), install).Times(0);
  EXPECT_CALL(*getAppEngine(), run).Times(0);

  updateApps(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed);
}

TEST_F(LiteClientTest, AppUpdateInstallFailure) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target that just adds a new an app
  auto new_target = createAppTarget({createApp("app-01")});

  ON_CALL(*getAppEngine(), run).WillByDefault(Return(false));

  // update to the latest version
  // fetch retry for three times
  EXPECT_CALL(*getAppEngine(), fetch).Times(1);
  EXPECT_CALL(*getAppEngine(), isRunning).Times(0);
  EXPECT_CALL(*getAppEngine(), install).Times(0);
  EXPECT_CALL(*getAppEngine(), run).Times(1);

  updateApps(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kOk,
             data::ResultCode::Numeric::kInstallFailed);
}

TEST_F(LiteClientTest, OstreeAndAppUpdateIfRollback) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  std::vector<AppEngine::App> apps{createApp("app-01")};
  auto target_01 = createTarget(&apps);

  {
    EXPECT_CALL(*getAppEngine(), fetch).Times(1);

    // since the Target/app is not installed then no reason to check if the app is running
    EXPECT_CALL(*getAppEngine(), isRunning).Times(0);

    // Just install no need too call run
    EXPECT_CALL(*getAppEngine(), install).Times(1);
    EXPECT_CALL(*getAppEngine(), run).Times(0);

    // update to the latest version
    update(*client, getInitialTarget(), target_01);
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
  }

  {
    std::vector<AppEngine::App> apps{createApp("app-01", "new-hash")};
    auto target_02 = createTarget(&apps);
    // update to the latest version
    update(*client, target_01, target_02);
    // deploy the previous version/commit to emulate rollback
    getSysRepo().deploy(target_01.sha256Hash());

    reboot(client);
    // make sure that a rollback has happened and a client is still running the previous Target
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
  }
}

/*
 * main
 */
int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  // options passed as args in CMakeLists.txt
  DeviceGatewayMock::RunCmd = argv[1];
  SysRootFS::CreateCmd = argv[2];
  return RUN_ALL_TESTS();
}
