#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "uptane_generator/image_repo.h"
#include "utilities/utils.h"

#include "utilities/utils.h"

#include "liteclient.h"
#include "composeappmanager.h"

#include <string>
#include <iostream>

#include "ostree/repo.h"
#include "helpers.h"


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
    process_{ServerPath, "-p", port_, "-d", repo_path} {
    LOG_INFO << "Treehub is running on port " << port_;
  }
  ~TreehubMock() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

  OSTreeMock& repo() { return repo_; }
  std::string url() { return "http://localhost:" + port_; }

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
        url_{"http://localhost:" + port_}, process_{ServerPath, port_, "-m", root_dir} {
    repo_.generateRepo(KeyType::kED25519);
    TestUtils::waitForServer(url_ + "/");
  }
  ~TufRepoMock() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

 public:
  const std::string& url() { return url_; }


  Uptane::Target add_target(const std::string& target_name, const std::string& hash,
                            const std::string& hardware_id, const std::string& target_version) {

    Delegation empty_delegetion{};
    Hash hash_obj{Hash::Type::kSha256, hash};
    Json::Value custom_json;
    custom_json["targetFormat"] = "OSTREE";
    custom_json["version"] = target_version;

    repo_.addCustomImage(target_name, hash_obj, 0, hardware_id, "", empty_delegetion, custom_json);

    Json::Value target;
    target["length"] = 0;
    target["hashes"]["sha256"] = hash;
    target["custom"] = custom_json;
    target["custom"]["version"] = target_version;

    return Uptane::Target(target_name, target);
  }

 private:
  ImageRepo repo_;
  std::string port_;
  std::string url_;
  boost::process::child process_;
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

    const std::string initial_version{"1"};
    const std::string target_name{hw_id + "-" + os + "-" + initial_version};
    auto update_commit_hash = treehub().repo().commit(sysrootfs().path, "lmp");
    auto initial_sysroot_commit_hash = sysrepo_.repo().commit(sysrootfs_.path, sysrootfs_.branch);

    if (initial_sysroot_commit_hash != update_commit_hash) {
      throw std::runtime_error("Initial commit to the system rootfs and the initial Target hash must be the same."
                               + initial_sysroot_commit_hash + " != " + update_commit_hash);
    }

    sysrepo_.deploy(initial_sysroot_commit_hash);
    initial_target_ = tufRepo().add_target(target_name, update_commit_hash, hw_id, initial_version);
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
    conf.pacman.ostree_server = treehub_.url();

    conf.bootloader.reboot_command = "/bin/true";
    conf.bootloader.reboot_sentinel_dir = conf.storage.path;
    conf.import.base_path = test_dir_ / "import";

    if (initial_version) {
      Json::Value ins_ver;
      ins_ver[initial_target_.sha256Hash()] = initial_target_.filename();
      std::string installed_version = Utils::jsonToCanonicalStr(ins_ver);
      Utils::writeFile(conf.import.base_path / "installed_versions", installed_version, true);
    }
    return std::make_shared<LiteClient>(conf);
  }

  Uptane::Target createNewTarget(const std::string& version_number) {
    // update rootfs and commit it into Treehub's repo
    const std::string unique_file = Utils::randomUuid();
    const std::string unique_content = Utils::randomUuid();
    Utils::writeFile(sysrootfs().path + "/" + unique_file, unique_content, true);
    auto update_commit_hash = treehub().repo().commit(sysrootfs().path, "lmp");

    // add new target to TUF repo
    const std::string target_name = hw_id + "-" + os + "-" + version_number;
    return tufRepo().add_target(target_name, update_commit_hash, hw_id, version_number);
  }

  bool areTargetsEqual(const Uptane::Target& lhs, const Uptane::Target& rhs) {
    return (lhs.sha256Hash() == rhs.sha256Hash()) && (lhs.filename() == rhs.filename());
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
  auto new_target = createNewTarget("2");

  // update to the latest version
  ASSERT_EQ(client->update(), data::ResultCode::Numeric::kNeedCompletion);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), initial_target()));
  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), new_target));

  // try to update to the latest version again, but it's already installed
  ASSERT_EQ(client->update(), data::ResultCode::Numeric::kAlreadyProcessed);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), new_target));
}

TEST_F(LiteClientTest, OstreeUpdateManual) {
  // boot device
  auto client = create_liteclient();
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));
  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createNewTarget("2");
  // forced update to a specific version
  ASSERT_EQ(client->update(new_target.filename(), true), data::ResultCode::Numeric::kNeedCompletion);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), initial_target()));
  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), new_target));

  // forced update to a specific version
  ASSERT_EQ(client->update(initial_target().filename(), true), data::ResultCode::Numeric::kNeedCompletion);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), new_target));
  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), initial_target()));

  // forced update to the same version again
  ASSERT_EQ(client->update(initial_target().filename(), true), data::ResultCode::Numeric::kAlreadyProcessed);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), initial_target()));
  // reboot device
  reboot(client);
  ASSERT_TRUE(areTargetsEqual(client->getCurrent(true), initial_target()));
}

//TEST_F(LiteClientTest, OstreeUpdateRollback) {
//  // boot device
//  auto client = create_liteclient();
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

//  // Create a new Target: update rootfs and commit it into Treehub's repo
//  auto new_target = createNewTarget("2");

//  // update
//  update(*client, initial_target(), new_target);

//  // deploy the initial version/commit to emulate rollback
//  sysrepo().deploy(initial_target().sha256Hash());
//  // reboot
//  reboot(client);
//  // make sure that a rollback has happened and a client is still running the initial Target
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

//  // make sure we cannot install the bad version
//  std::vector<Uptane::Target> known_but_not_installed_versions;
//  get_known_but_not_installed_versions(*client, known_but_not_installed_versions);
//  ASSERT_TRUE(known_local_target(*client, new_target, known_but_not_installed_versions));

//  // make sure we can update a device with a new valid Target
//  auto new_target_03 = createNewTarget("3");

//  // update
//  update(*client, initial_target(), new_target_03);

//  // reboot
//  reboot(client);
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target_03));
//}

//TEST_F(LiteClientTest, OstreeUpdateToLatestAfterManualUpdate) {
//  // boot device
//  auto client = create_liteclient();
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

//  // Create a new Target: update rootfs and commit it into Treehub's repo
//  auto new_target = createNewTarget("2");

//  // update
//  update(*client, initial_target(), new_target);

//  // reboot device
//  reboot(client);
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target));

//  // emulate manuall update to the previous version
//  update(*client, new_target, initial_target());

//  // reboot device and make sure that the previous version is installed
//  reboot(client);
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), initial_target()));

//  // make sure we can install the latest version that has been installed before
//  // the succesfully installed Target should be "not known"
//  std::vector<Uptane::Target> known_but_not_installed_versions;
//  get_known_but_not_installed_versions(*client, known_but_not_installed_versions);
//  ASSERT_FALSE(known_local_target(*client, new_target, known_but_not_installed_versions));

//  // emulate auto update to the latest
//  update(*client, initial_target(), new_target);

//  // reboot device
//  reboot(client);
//  ASSERT_TRUE(areTargetsEqual(client->getCurrent(), new_target));
//}


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
