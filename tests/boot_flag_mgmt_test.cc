#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <boost/algorithm/string.hpp>
#include <boost/process.hpp>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "appengine.h"
#include "composeappmanager.h"
#include "liteclient.h"
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
    ON_CALL(*this, verify).WillByDefault(Return(true));
    ON_CALL(*this, install).WillByDefault(Return(true));
    ON_CALL(*this, run).WillByDefault(Return(true));
    ON_CALL(*this, isFetched).WillByDefault(Return(true));
    ON_CALL(*this, isRunning).WillByDefault(Return(true));
    ON_CALL(*this, getRunningAppsInfo)
        .WillByDefault(
            Return(Utils::parseJSON("{\"app-07\": {\"services\": {\"nginx-07\": {\"hash\": "
                                    "\"16e36b4ab48cb19c7100a22686f85ffcbdce5694c936bda03cb12a2cce88efcf\"}}}}")));
  }

 public:
  MOCK_METHOD(AppEngine::Result, fetch, (const App& app), (override));
  MOCK_METHOD(AppEngine::Result, verify, (const App& app), (override));
  MOCK_METHOD(AppEngine::Result, install, (const App& app), (override));
  MOCK_METHOD(AppEngine::Result, run, (const App& app), (override));
  MOCK_METHOD(void, stop, (const App& app), (override));
  MOCK_METHOD(void, remove, (const App& app), (override));
  MOCK_METHOD(bool, isFetched, (const App& app), (const, override));
  MOCK_METHOD(bool, isRunning, (const App& app), (const, override));
  MOCK_METHOD(Json::Value, getRunningAppsInfo, (), (const, override));
  MOCK_METHOD(void, prune, (const Apps& app), (override));
};

class BootFlagMgmtTest : public fixtures::ClientTest {
 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               bool finalize = true) override {
    app_engine_mock_ = std::make_shared<NiceMock<MockAppEngine>>();

    return ClientTest::createLiteClient(app_engine_mock_, initial_version, apps, "", boost::none, true, finalize);
  }

  std::shared_ptr<NiceMock<MockAppEngine>>& getAppEngine() { return app_engine_mock_; }

 private:
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_mock_;
};

class BootFlagMgmtTestSuite : public BootFlagMgmtTest,
                              public ::testing::WithParamInterface<std::tuple<std::string, RollbackMode>> {
 protected:
  void tweakConf(Config& conf) override {
    std::string pacman_type;
    RollbackMode bootloader_mode;

    std::tie(pacman_type, bootloader_mode) = GetParam();
    conf.pacman.type = pacman_type;
    conf.bootloader.rollback_mode = bootloader_mode;
  };
};

TEST_P(BootFlagMgmtTestSuite, OstreeUpdate) {
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

INSTANTIATE_TEST_SUITE_P(
    BootFlagMgmtTestSuiteParam, BootFlagMgmtTestSuite,
    ::testing::Values(std::tuple<std::string, RollbackMode>{"ostree", RollbackMode::kUbootGeneric},
                      std::tuple<std::string, RollbackMode>{"ostree", RollbackMode::kUbootMasked},
                      std::tuple<std::string, RollbackMode>{"ostree", RollbackMode::kFioVB},
                      std::tuple<std::string, RollbackMode>{"ostree+compose_apps", RollbackMode::kUbootGeneric},
                      std::tuple<std::string, RollbackMode>{"ostree+compose_apps", RollbackMode::kUbootMasked},
                      std::tuple<std::string, RollbackMode>{"ostree+compose_apps", RollbackMode::kFioVB}));

class BootUpgradeFlagMgmtTestSuite : public BootFlagMgmtTestSuite {};

TEST_P(BootUpgradeFlagMgmtTestSuite, OstreeAndBootloaderUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // First update
  {
    // Create a new Target: update rootfs and commit it into Treehub's repo
    auto new_target =
        createTarget(nullptr, "", "", boost::none, "", std::string(bootloader::BootloaderLite::VersionTitle) + ":1.1");
    update(*client, getInitialTarget(), new_target);

    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    checkHeaders(*client, new_target);
  }

  // make sure `bootupgrade_available` is set to 1 after the first successful reboot
  ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 1);

  // Second update
  {
    // Create a new Target: update rootfs and commit it into Treehub's repo
    auto new_target =
        createTarget(nullptr, "", "", boost::none, "", std::string(bootloader::BootloaderLite::VersionTitle) + ":1.2");
    update(*client, client->getCurrent(), new_target);
    // make sure `bootupgrade_available` is set to 2 after the second successful update
    ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 2);
    // emulate the bootloader reset of `bootupgrade_available` to 0
    boot_flag_mgr_->set_bootupgrade_available(0);
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    checkHeaders(*client, new_target);
    ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 0);
  }
}

TEST_P(BootUpgradeFlagMgmtTestSuite, OstreeAndBootloaderUpdateFollowedByRollback) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // First update
  {
    // Create a new Target: update rootfs and commit it into Treehub's repo
    auto new_target =
        createTarget(nullptr, "", "", boost::none, "", std::string(bootloader::BootloaderLite::VersionTitle) + ":1.1");
    update(*client, getInitialTarget(), new_target);

    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    checkHeaders(*client, new_target);
  }

  // make sure `bootupgrade_available` is set to 1 after the first successful reboot
  ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 1);

  // Second update
  {
    const auto initialTarget{client->getCurrent()};
    // Create a new Target: update rootfs and commit it into Treehub's repo
    auto new_target =
        createTarget(nullptr, "", "", boost::none, "", std::string(bootloader::BootloaderLite::VersionTitle) + ":1.2");
    update(*client, client->getCurrent(), new_target);
    // make sure `bootupgrade_available` is set to 2 after the second successful update
    ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 2);

    // emulate rollback caused by App installation failure
    client->install(initialTarget);

    ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 1);
  }
}

TEST_P(BootUpgradeFlagMgmtTestSuite, OstreeUpdateIfNoBootloaderUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // First update
  {
    // Create a new Target: update rootfs and commit it into Treehub's repo
    auto new_target =
        createTarget(nullptr, "", "", boost::none, "", std::string(bootloader::BootloaderLite::VersionTitle) + ":1.1");
    update(*client, getInitialTarget(), new_target);

    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    checkHeaders(*client, new_target);
  }

  // make sure `bootupgrade_available` is set to 1 after the first successful reboot
  ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 1);

  // Second update
  {
    const auto initialTarget{client->getCurrent()};
    // Create a new Target: update rootfs and commit it into Treehub's repo
    // No boot fw update, it' still 1.1
    auto new_target =
        createTarget(nullptr, "", "", boost::none, "", std::string(bootloader::BootloaderLite::VersionTitle) + ":1.1");
    update(*client, client->getCurrent(), new_target);
    // make sure `bootupgrade_available` is still 1 after the second successful update which does not include the boot
    // fw update
    ASSERT_EQ(boot_flag_mgr_->bootupgrade_available(), 1);
  }
}

INSTANTIATE_TEST_SUITE_P(
    BootUpgradeFlagMgmtTestParam, BootUpgradeFlagMgmtTestSuite,
    ::testing::Values(std::tuple<std::string, RollbackMode>{"ostree", RollbackMode::kUbootMasked},
                      std::tuple<std::string, RollbackMode>{"ostree", RollbackMode::kFioVB},
                      std::tuple<std::string, RollbackMode>{"ostree+compose_apps", RollbackMode::kUbootMasked},
                      std::tuple<std::string, RollbackMode>{"ostree+compose_apps", RollbackMode::kFioVB}));

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
