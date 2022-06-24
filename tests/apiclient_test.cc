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

#include "aktualizr-lite/api.h"
#include "composeappmanager.h"
#include "liteclient.h"

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
  MOCK_METHOD(bool, fetch, (const App& app), (override));
  MOCK_METHOD(bool, verify, (const App& app), (override));
  MOCK_METHOD(bool, install, (const App& app), (override));
  MOCK_METHOD(bool, run, (const App& app), (override));
  MOCK_METHOD(void, stop, (const App& app), (override));
  MOCK_METHOD(void, remove, (const App& app), (override));
  MOCK_METHOD(bool, isFetched, (const App& app), (const, override));
  MOCK_METHOD(bool, isRunning, (const App& app), (const, override));
  MOCK_METHOD(Json::Value, getRunningAppsInfo, (), (const, override));
  MOCK_METHOD(void, prune, (const Apps& app), (override));
};

class ApiClientTest : public fixtures::ClientTest {
 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               bool finalize = true) override {
    app_engine_mock_ = std::make_shared<NiceMock<MockAppEngine>>();
    return ClientTest::createLiteClient(app_engine_mock_, initial_version, apps);
  }

  std::shared_ptr<NiceMock<MockAppEngine>>& getAppEngine() { return app_engine_mock_; }

 private:
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_mock_;
};

TEST_F(ApiClientTest, GetConfig) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));
  ASSERT_EQ("\"ostree+compose_apps\"", client.GetConfig().get("pacman.type", ""));
}

TEST_F(ApiClientTest, GetCurrent) {
  auto cur = AkliteClient(createLiteClient(InitialVersion::kOff)).GetCurrent();
  ASSERT_EQ("unknown", cur.Name());
  ASSERT_EQ(-1, cur.Version());
}

TEST_F(ApiClientTest, CheckIn) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));

  auto result = client.CheckIn();

  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ(2, events.size());
  auto val = getDeviceGateway().readSotaToml();
  ASSERT_NE(std::string::npos, val.find("[pacman]"));

  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(0, result.Targets().size());

  ASSERT_TRUE(getDeviceGateway().resetSotaToml());
  ASSERT_TRUE(getDeviceGateway().resetEvents());

  auto new_target = createTarget();
  result = client.CheckIn();
  ASSERT_EQ(0, getDeviceGateway().getEvents().size());
  ASSERT_EQ("", getDeviceGateway().readSotaToml());
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(1, result.Targets().size());
  ASSERT_EQ(new_target.filename(), result.Targets()[0].Name());
  ASSERT_EQ(new_target.sha256Hash(), result.Targets()[0].Sha256Hash());
}

TEST_F(ApiClientTest, Rollback) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*liteclient, getInitialTarget(), new_target);

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_FALSE(client.IsRollback(result.GetLatest()));

  // deploy the initial version/commit to emulate rollback
  getSysRepo().deploy(getInitialTarget().sha256Hash());

  reboot(liteclient);
  // reboot re-creates an instance of LiteClient so `client` refers to an invalid/removed instance of LiteClient now,
  // hence we need to re-create an instance of AkliteClient
  AkliteClient rebooted_client(liteclient);

  ASSERT_TRUE(rebooted_client.IsRollback(result.GetLatest()));
  ASSERT_EQ(rebooted_client.GetCurrent().Sha256Hash(), getInitialTarget().sha256Hash());
}

TEST_F(ApiClientTest, Install) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  auto latest = result.GetLatest();

  auto installer = client.Installer(latest);
  ASSERT_NE(nullptr, installer);
  auto dresult = installer->Download();
  ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

  auto iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);
}

TEST_F(ApiClientTest, InstallWithCorrelationId) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  auto latest = result.GetLatest();

  getDeviceGateway().resetEvents();

  auto installer = client.Installer(latest, "", "this-is-random");
  ASSERT_NE(nullptr, installer);
  auto dresult = installer->Download();
  ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

  auto iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);

  ASSERT_EQ("this-is-random", installer->GetCorrelationId());
  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ("this-is-random", events[0]["event"]["correlationId"].asString());
}

TEST_F(ApiClientTest, Secondaries) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));
  std::vector<SecondaryEcu> ecus;
  ecus.emplace_back("123", "riscv", "target12");
  auto res = client.SetSecondaries(ecus);
  ASSERT_EQ(InstallResult::Status::Ok, res.status);
  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ(1, events.size());
  ASSERT_EQ("target12", events[0]["123"]["target"].asString());
  ASSERT_EQ("riscv", events[0]["123"]["hwid"].asString());

  auto new_target = createTarget();
  auto secondary_target = createTarget(nullptr, "riscv");
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  ASSERT_EQ(2, result.Targets().size());
  ASSERT_EQ(new_target.filename(), result.GetLatest().Name());
  ASSERT_EQ(secondary_target.filename(), result.GetLatest("riscv").Name());
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
