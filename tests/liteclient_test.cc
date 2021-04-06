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

#include "docker/composeappengine.h"
#include "helpers.h"
#include "ostree/repo.h"
#include "target.h"

#include "fixtures/liteclienttest.cc"

using ::testing::NiceMock;
using ::testing::Return;

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

class LiteClientTest : public fixtures::ClientTest {
 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none) override {
    app_engine_mock_ = std::make_shared<NiceMock<MockAppEngine>>();
    return ClientTest::createLiteClient(app_engine_mock_, initial_version, apps);
  }

  std::shared_ptr<NiceMock<MockAppEngine>>& getAppEngine() { return app_engine_mock_; }

 private:
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_mock_;
};

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
    std::vector<AppEngine::App> apps{createApp("app-01", "test-factory", "new-hash")};
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
  fixtures::DeviceGatewayMock::RunCmd = argv[1];
  fixtures::SysRootFS::CreateCmd = argv[2];
  return RUN_ALL_TESTS();
}
