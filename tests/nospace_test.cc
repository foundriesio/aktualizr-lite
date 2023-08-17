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

#include "fixtures/liteclienttest.cc"

// Defined in fstatvfs-mock.cc
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

 private:
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_mock_;
};

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
