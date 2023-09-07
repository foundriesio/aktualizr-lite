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

#include "fixtures/aklitetest.cc"

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
    ASSERT_TRUE(std::string::npos != usage_info.str().find("required: 7B unknown%")) << usage_info;
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
    usage_info.free.second = std::round(usage_info.free.second);
    usage_info.available.second = std::round(usage_info.available.second);
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
    usage_info.free.second = std::round(usage_info.free.second);
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
  const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
  ASSERT_TRUE(std::string::npos != event_err_msg.find("min-free-space-size 1048576MB would be exceeded"))
      << event_err_msg;
  ASSERT_TRUE(std::string::npos != event_err_msg.find("before ostree pull; available:")) << event_err_msg;
  ASSERT_TRUE(std::string::npos != event_err_msg.find("after ostree pull; available:")) << event_err_msg;

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

  {
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("min-free-space-percent '50%' would be exceeded"))
        << event_err_msg;
    ASSERT_TRUE(std::string::npos != event_err_msg.find("available: 4096B 1%")) << event_err_msg;
  }
  {
    // Now, let's decrease the required minimum space to 40%, since the update size is < 9 blocks,
    // then the libostree should be happy.
    // We need to "reboot" in order to recreate the client instance so the new watermark is applied.
    setMinFreeSpace("40");
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
    update(*client, getInitialTarget(), new_target);
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  }
  UnsetFreeBlockNumb();
}

TEST_F(NoSpaceTest, OstreeUpdateNoSpaceIfStaticDelta) {
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  // Delta size is 3 + 1 = 4 blocks
  setGenerateStaticDelta(3);
  auto new_target = createTarget();
  {
    // The delta-based update if there is no stat/info about the delta, so the pre-pull verification
    // of the update size is not possible. Thus, the error originates in libostree; libostree does NOT
    // apply any threshold/reserved when checking if there is enough storage to store a delta file,
    // it just checks for the overall storage capacity.
    //
    // required 4%, free 2%, available 0%, no pre-pull check -> libostree generates the error "Delta requires..."
    // The expected libostree error:
    //    "Error while pulling image: 0 Delta requires 13.8 kB free space, but only 4.1 kB available"
    SetFreeBlockNumb(2, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("Delta requires")) << event_err_msg;
    // libostree converts 2*4096=8192B by dividing it to 1000 not 1024.
    ASSERT_TRUE(std::string::npos != event_err_msg.find("free space, but only 8.2 kB available")) << event_err_msg;
    ASSERT_TRUE(std::string::npos != event_err_msg.find("available: 0B 0%")) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find(OSTree::Sysroot::Config::ReservedStorageSpacePercentageOstreeParamName))
        << event_err_msg;
  }
  {
    // The delta-based update if there is no stat/info about the delta, so the pre-pull verification
    // of the update size is not possible. Thus, the error originates in libostree; libostree does NOT
    // apply any threshold/reserved when checking if there is enough storage to store a delta file,
    // it just checks for the overall storage capacity.
    // In this case, there is enough free storage to accommodate the delta file.
    // But, during committing the ostree objects extracted from the delta file, libostree checks
    // if there is enough free storage is available taking into account the
    // `min-free-space-percent`/`min-free-space-size` threshold -> reserved storage. By default, the libostree sets it
    // to 3%.
    //
    // required 4%, free 5%, reserved 3%, available 2%, no pre-pull check -> libostree generates the error "would be
    // exceeded, at least..." Expected error message is:
    //    "Error while pulling image: 0 opcode close: min-free-space-percent '3%' would be exceeded, at least 13 bytes
    //    requested"
    SetFreeBlockNumb(5, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("min-free-space-percent '3%' would be exceeded, at least"))
        << event_err_msg;
    ASSERT_TRUE(std::string::npos != event_err_msg.find("available: 8192B 2%")) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find(OSTree::Sysroot::Config::ReservedStorageSpacePercentageOstreeParamName));
  }
  {
    // required 4%, free 7%, reserved 3%, available 7% - 3% = 4% -> should be ok,
    // but there is a moment during the delta-based pull when libostree has the delta file
    // on a file system + extracted files while it commits the extracted files to the repo.
    // So, it takes into account the delta file size + extracted objects during ostree objects committing,
    // therefore we need 4% + <some additional space> ~ 5% (required)+ 3% (reserved) ~ 8% (at least 8 free blocks
    // is required, but just 7 is available)
    //
    storage::Volume::UsageInfo usage_info{.size = {100 * 4096, 100}, .available = {(7 - 3) * 4096, 7 - 3}};
    std::stringstream expected_available;
    expected_available << usage_info.available;
    SetFreeBlockNumb(7, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("min-free-space-percent '3%' would be exceeded, at least"))
        << event_err_msg;
    ASSERT_TRUE(std::string::npos != event_err_msg.find(expected_available.str())) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find(OSTree::Sysroot::Config::ReservedStorageSpacePercentageOstreeParamName));
  }
  {
    // required 4%, free 15%, reserved 3%, available 15% - 3% = 12% -> ok
    SetFreeBlockNumb(14, 100);
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
  const auto delta_size{getDeltaSize(getInitialTarget(), new_target)};
  {
    // not enough free blocks
    SetFreeBlockNumb(5, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    // required 11%, free 15%, reserved 5% (default, by the delta knob) -> available 10% < 11%
    SetFreeBlockNumb(15, 100);
    const auto expected_available{((15 > OSTree::Sysroot::Config::DefaultReservedStorageSpacePercentageDelta))
                                      ? (15 - OSTree::Sysroot::Config::DefaultReservedStorageSpacePercentageDelta)
                                      : 0};
    storage::Volume::UsageInfo usage_info{.size = {100 * 4096, 100},
                                          .available = {expected_available * 4096, expected_available}};
    std::stringstream expected_msg;
    expected_msg << "required: " << usage_info.withRequired(delta_size).required
                 << ", available: " << usage_info.available;
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const std::string event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find(expected_msg.str())) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find(OSTree::Sysroot::Config::ReservedStorageSpacePercentageDeltaParamName))
        << event_err_msg;
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    // required 11%, free 16%, reserved 5% by the delta knob, reserved 6% by the ostree knob
    // -> available 10% < 11%
    SetFreeBlockNumb(16, 100);
    sys_repo_.setMinFreeSpacePercent("6");
    const auto expected_available{16 - 6};
    storage::Volume::UsageInfo usage_info{.size = {100 * 4096, 100},
                                          .available = {expected_available * 4096, expected_available}};
    std::stringstream expected_msg;
    expected_msg << "required: " << usage_info.withRequired(delta_size).required
                 << ", available: " << usage_info.available;
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const std::string event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find(expected_msg.str())) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find(OSTree::Sysroot::Config::ReservedStorageSpacePercentageOstreeParamName))
        << event_err_msg;
    ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  }
  {
    sys_repo_.setMinFreeSpacePercent("1");
    SetFreeBlockNumb(21, 100);
    update(*client, getInitialTarget(), new_target);
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    const std::string msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != msg.find("before ostree pull")) << msg;
    ASSERT_TRUE(std::string::npos != msg.find("after ostree pull")) << msg;
  }
  UnsetFreeBlockNumb();
}

class AkliteNoSpaceTest : public AkliteTest {
 protected:
  Docker::RestorableAppEngine::StorageSpaceFunc getTestStorageSpaceFunc() override {
    // Use the restorable app engine default storage usage function since
    // `fstatvfs` is mocked in the `AkliteNoSpaceTest` based tests.
    return Docker::RestorableAppEngine::GetDefStorageSpaceFunc();
  }
};

TEST_P(AkliteNoSpaceTest, OstreeAndAppUpdateNotEnoughSpaceForApps) {
  // App's containers are re-created before reboot
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  std::vector<AppEngine::App> apps{app01};
  auto new_target = createTarget(&apps);

  {
    // Not enough free space to pull an App bundle/archive since there is only 20% of free space
    // and 20% is reserved, so 0% is available for the update
    SetFreeBlockNumb(20, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("store: skopeo apps")) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find("free: 81920B 20%, reserved: 81920B 20%(by `pacman:storage_watermark`)"))
        << event_err_msg;
  }
  {
    // Enough free space to pull an App bundle/archive since there is 21 - 20% of free space.
    // But, it's not enough available free space to pull the App image because
    // the App image requires more than 1 block.
    SetFreeBlockNumb(21, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("store: skopeo")) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find("free: 86016B 21%, reserved: 81920B 20%(by `pacman:storage_watermark`)"))
        << event_err_msg;
  }
  {
    // Enough free space to pull an App bundle/archive and the App image layers/blobs.
    // But, it's not enough available free space to accommodate the App in the docker store (extracted image layers).
    SetFreeBlockNumb(36, 100);
    update(*client, getInitialTarget(), new_target, data::ResultCode::Numeric::kDownloadFailed,
           {DownloadResult::Status::DownloadFailed_NoSpace, "Insufficient storage available"});
    const auto event_err_msg{getEventContext("EcuDownloadCompleted")};
    ASSERT_TRUE(std::string::npos != event_err_msg.find("store: docker")) << event_err_msg;
    ASSERT_TRUE(std::string::npos !=
                event_err_msg.find("free: 147456B 36%, reserved: 81920B 20%(by `pacman:storage_watermark`)"))
        << event_err_msg;
  }
  UnsetFreeBlockNumb();
}

INSTANTIATE_TEST_SUITE_P(MultiEngine, AkliteNoSpaceTest, ::testing::Values("RestorableAppEngine"));

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
