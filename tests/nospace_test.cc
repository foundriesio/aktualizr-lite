#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <sys/statvfs.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "composeappmanager.h"
#include "liteclient.h"
#include "storage/stat.h"

#include "fixtures/liteclienttest.cc"

// Defined in fstatvfs-mock.cc
extern void SetBlockSize(unsigned long int);
extern void SetFreeBlockNumb(uint64_t, uint64_t);
extern void UnsetFreeBlockNumb();

using ::testing::NiceMock;
using ::testing::Return;

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
  MOCK_METHOD(AppEngine::Apps, getInstalledApps, (), (const, override));
  MOCK_METHOD(Json::Value, getRunningAppsInfo, (), (const, override));
  MOCK_METHOD(void, prune, (const Apps& app), (override));
};

class NoSpaceTest : public fixtures::ClientTest {
 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               bool finalize = true) override {
    app_engine_mock_ = std::make_shared<NiceMock<MockAppEngine>>();
    return ClientTest::createLiteClient(app_engine_mock_, initial_version, apps, "", boost::none, true, finalize);
  }

  std::shared_ptr<NiceMock<MockAppEngine>>& getAppEngine() { return app_engine_mock_; }

  void tweakConf(Config& cfg) override {
    if (!min_free_space_.empty()) {
      cfg.pacman.extra[OSTree::Sysroot::Config::ReservedStorageSpacePercentageOstreeParamName] = min_free_space_;
    }
  }
  void setMinFreeSpace(const std::string& min_free_space) { min_free_space_ = min_free_space; }

 private:
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_mock_;
  std::string min_free_space_;
};

TEST_F(NoSpaceTest, ReservedStorageSpacePercentageDeltaParam) {
  {
    // check default value
    const auto cfg{OSTree::Sysroot::Config(PackageConfig{})};
    ASSERT_EQ(OSTree::Sysroot::Config::DefaultReservedStorageSpacePercentageDelta,
              cfg.ReservedStorageSpacePercentageDelta);
  }
  {
    // check if set to the default value if the specified param value is ivalid
    PackageConfig pacmancfg;
    pacmancfg.extra[OSTree::Sysroot::Config::ReservedStorageSpacePercentageDeltaParamName] = "10foo";
    const auto cfg{OSTree::Sysroot::Config(pacmancfg)};
    ASSERT_EQ(OSTree::Sysroot::Config::DefaultReservedStorageSpacePercentageDelta,
              cfg.ReservedStorageSpacePercentageDelta);
  }
  {
    // check if set to the min allowed value if the specified param value is lower than the one
    PackageConfig pacmancfg;
    pacmancfg.extra[OSTree::Sysroot::Config::ReservedStorageSpacePercentageDeltaParamName] =
        std::to_string(OSTree::Sysroot::Config::MinReservedStorageSpacePercentageDelta - 1);
    const auto cfg{OSTree::Sysroot::Config(pacmancfg)};
    ASSERT_EQ(OSTree::Sysroot::Config::MinReservedStorageSpacePercentageDelta, cfg.ReservedStorageSpacePercentageDelta);
  }
  {
    // check if set to the max allowed value if the specified param value is higher than the one
    PackageConfig pacmancfg;
    pacmancfg.extra[OSTree::Sysroot::Config::ReservedStorageSpacePercentageDeltaParamName] =
        std::to_string(OSTree::Sysroot::Config::MaxReservedStorageSpacePercentageDelta + 1);
    const auto cfg{OSTree::Sysroot::Config(pacmancfg)};
    ASSERT_EQ(OSTree::Sysroot::Config::MaxReservedStorageSpacePercentageDelta, cfg.ReservedStorageSpacePercentageDelta);
  }
  {
    // check if a custom valid value can be set
    PackageConfig pacmancfg;
    const unsigned int my_watermark{43};
    pacmancfg.extra[OSTree::Sysroot::Config::ReservedStorageSpacePercentageDeltaParamName] =
        std::to_string(my_watermark);
    const auto cfg{OSTree::Sysroot::Config(pacmancfg)};
    ASSERT_EQ(my_watermark, cfg.ReservedStorageSpacePercentageDelta);
  }
}

TEST(StorageStat, UsageInfo) {
  {
    storage::Volume::UsageInfo usage_info{storage::Volume::getUsageInfo("non-existing-path", 5)};
    ASSERT_FALSE(usage_info.isOk());
    ASSERT_FALSE(usage_info.err.empty()) << usage_info.err;
    usage_info.withRequired(7);
    ASSERT_EQ(7, usage_info.required.first);
    ASSERT_EQ(0, usage_info.required.second);
    ASSERT_EQ("required: 7B unknown%", usage_info.str());
  }
  {
    unsigned int block_size{4096};
    uint64_t block_numb{100};
    unsigned int free_percentage{15};
    unsigned int reserved_percentage{10};
    const auto reserved_by{"ostree_min_free_space"};

    storage::Volume::UsageInfo::Type free{std::ceil(block_numb * (free_percentage / 100.0)) * block_size,
                                          free_percentage};
    storage::Volume::UsageInfo::Type reserved{std::ceil(block_numb * (reserved_percentage / 100.0)) * block_size,
                                              reserved_percentage};
    storage::Volume::UsageInfo::Type used{std::ceil(block_numb * ((100 - free_percentage) / 100.0)) * block_size,
                                          (100 - free_percentage)};

    SetBlockSize(block_size);
    SetFreeBlockNumb(std::ceil(block_numb * (free_percentage / 100.0)), block_numb);
    storage::Volume::UsageInfo usage_info{storage::Volume::getUsageInfo("./", reserved_percentage, reserved_by)};
    ASSERT_TRUE(usage_info.isOk());
    ASSERT_EQ(free, usage_info.free) << usage_info.free.first;
    ASSERT_EQ(reserved, usage_info.reserved) << usage_info.free.first;
    ASSERT_EQ((free.first - reserved.first), usage_info.available.first) << usage_info.available.first;
    ASSERT_EQ((free.second - reserved.second), usage_info.available.second) << usage_info.available.second;
    ASSERT_EQ(reserved_by, usage_info.reserved_by) << usage_info.reserved_by;
  }
  {
    // The same amount of free and reserved space
    unsigned int block_size{4096};
    uint64_t block_numb{999};
    unsigned int free_percentage{15};
    unsigned int reserved_percentage{15};

    storage::Volume::UsageInfo::Type free{std::ceil(block_numb * (free_percentage / 100.0)) * block_size,
                                          free_percentage};
    storage::Volume::UsageInfo::Type reserved{std::ceil(block_numb * (reserved_percentage / 100.0)) * block_size,
                                              reserved_percentage};
    storage::Volume::UsageInfo::Type used{std::ceil(block_numb * ((100 - free_percentage) / 100.0)) * block_size,
                                          (100 - free_percentage)};

    SetBlockSize(block_size);
    SetFreeBlockNumb(std::ceil(block_numb * (free_percentage / 100.0)), block_numb);
    storage::Volume::UsageInfo usage_info{storage::Volume::getUsageInfo("./", reserved_percentage)};
    ASSERT_TRUE(usage_info.isOk());
    ASSERT_EQ(free, usage_info.free) << usage_info.free.first;
    ASSERT_EQ(reserved, usage_info.reserved) << usage_info.reserved.first;
    ASSERT_EQ((free.first - reserved.first), usage_info.available.first) << usage_info.available.first;
    ASSERT_EQ((free.second - reserved.second), usage_info.available.second) << usage_info.available.second;
    ASSERT_EQ(storage::Volume::UsageInfo::Type(0, 0), usage_info.available) << usage_info.available.first;
  }
  {
    // The amount of free space is less than the required reserved
    unsigned int block_size{1024};
    uint64_t block_numb{999};
    unsigned int free_percentage{13};
    unsigned int reserved_percentage{15};

    storage::Volume::UsageInfo::Type free{std::ceil(block_numb * (free_percentage / 100.0)) * block_size,
                                          free_percentage};
    storage::Volume::UsageInfo::Type reserved{std::ceil(block_numb * (reserved_percentage / 100.0)) * block_size,
                                              reserved_percentage};
    storage::Volume::UsageInfo::Type used{std::ceil(block_numb * ((100 - free_percentage) / 100.0)) * block_size,
                                          (100 - free_percentage)};

    SetBlockSize(block_size);
    SetFreeBlockNumb(std::ceil(block_numb * (free_percentage / 100.0)), block_numb);
    storage::Volume::UsageInfo usage_info{storage::Volume::getUsageInfo("./", reserved_percentage)};
    ASSERT_TRUE(usage_info.isOk());
    ASSERT_EQ(free, usage_info.free) << usage_info.free.first;
    ASSERT_EQ(reserved, usage_info.reserved) << usage_info.reserved.first;
    ASSERT_EQ(storage::Volume::UsageInfo::Type(0, 0), usage_info.available) << usage_info.available.first;
  }
  UnsetFreeBlockNumb();
}

TEST_F(NoSpaceTest, OstreeUpdateNoSpace) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  sys_repo_.setMinFreeSpace("1TB");
  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
         {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // reboot device
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  checkHeaders(*client, getInitialTarget());
}

TEST_F(NoSpaceTest, OstreeUpdateNoSpaceIfWatermarkParamIsSet) {
  // There 51% of blocks are free. The update takes a few blocks,
  // so the pull should fail since the storage usage exceeds
  // the set required minimum space  - 50%.
  SetFreeBlockNumb(51, 100);
  setMinFreeSpace("50");
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
         {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Now, let's decrease the required minimum space to 40%, since the update size is < 9 blocks,
  // then the libostree should be happy.
  // We need to "reboot" in order to recreate the client instance so the new watermark is applied.
  setMinFreeSpace("40");
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  update(*client, getInitialTarget(), new_target);
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  UnsetFreeBlockNumb();
}

TEST_F(NoSpaceTest, OstreeUpdateNoSpaceIfStaticDelta) {
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  setGenerateStaticDelta(3);
  auto new_target = createTarget();
  {
    // Not enough space/block, 3 is required, we got only 1.
    // The expected libostree error:
    //    "Error while pulling image: 0 Delta requires 13.8 kB free space, but only 4.1 kB available"
    SetFreeBlockNumb(1, 10);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    // There is enough space to accommodate the 3-block delta since we have 3 free blocks.
    // But, at the last step of the delta processing libostree calls dispatch_close() ->
    // _ostree_repo_bare_content_commit(). And, the _ostree_repo_bare_content_commit() checks the
    // `min-free-space-percent`/`min-free-space-size` watermark. In our case we have 3 out of 100 blocks free,
    // which is enough to fit the delta, but less than the `min-free-space-percent` of overall storage
    // becomes free after the update, so libostree rejects it.
    // It  doesn't make sense to reject the update after its content is pulled and already written to a disk,
    // but this is the way libostree works, so we have to adjust...(we bypass this issue by using the delta stats)
    // Expected error message is:
    //    "Error while pulling image: 0 opcode close: min-free-space-percent '3%' would be exceeded, at least 13 bytes
    //    requested"
    SetFreeBlockNumb(3, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    // Now, we have 15 blocks out of 100 free, so 15-3 = 12 - 12% of storage will be free after the update,
    // so libostree should be happy.
    // NOTE: 7 blocks (7%) should suffice, but for some reason libostree requires 15%.
    // TODO: Check the following assumption.
    //       Most likely there is a moment during download at which 2x of the update/delta size is required.
    //       Specifically, in this case, 3% for downloaded delta and then 3% to commit it to the repo - hence > 6% is
    //       needed.
    SetFreeBlockNumb(15, 100);
    update(*client, getInitialTarget(), new_target);
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  }
  UnsetFreeBlockNumb();
}

TEST_F(NoSpaceTest, OstreeUpdateNoSpaceIfStaticDeltaStats) {
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  // Delta size will be 10 + 1 = 11 blocks, 1  block for additional data like boot loader version file.
  setGenerateStaticDelta(10, true);
  auto new_target = createTarget();
  {
    // not enough free blocks
    SetFreeBlockNumb(5, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    // not enough free blocks taking into account the default reserved 5%,
    // (15 - 11 = 4) - 4% of blocks will be free after the update, we need 5%
    SetFreeBlockNumb(15, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const auto events{device_gateway_.getEvents()};
    const std::string event_err_msg{events[events.size() - 1]["event"]["details"].asString()};
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find("available 40960 out of 389120(95% of the volume capacity 409600)"))
        << event_err_msg;
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    // (21 - 11 = 10) - 10% of blocks will be free, need 10% so, it's supposed to succeed.
    // But, the commit function checks if it will be more than 15% of storage capacity free after commit.
    // Obviously it's not since only 10% will be available.
    SetFreeBlockNumb(21, 100);
    sys_repo_.setMinFreeSpacePercent("15");
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const auto events{device_gateway_.getEvents()};
    const std::string event_err_msg{events[events.size() - 1]["event"]["details"].asString()};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("opcode close: min-free-space-percent '15%' would be exceeded"))
        << event_err_msg;
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    sys_repo_.setMinFreeSpacePercent("1");
    SetFreeBlockNumb(21, 100);
    update(*client, getInitialTarget(), new_target);
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  }
  UnsetFreeBlockNumb();
}

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
