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

const char* aklite_bin;

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
  MOCK_METHOD(void, remove, (const App& app), (override));
  MOCK_METHOD(bool, isFetched, (const App& app), (const, override));
  MOCK_METHOD(bool, isRunning, (const App& app), (const, override));
  MOCK_METHOD(Json::Value, getRunningAppsInfo, (), (const, override));
  MOCK_METHOD(void, prune, (const Apps& app), (override));
};

class ApiServerTest : public fixtures::ClientTest {
 public:
  ~ApiServerTest() {
    if (aklite_ != nullptr) {
      LOG_INFO << "Stopping aklite server";
      aklite_->terminate();
      aklite_->wait_for(std::chrono::seconds(10));
    }
  }
  void startServer() {
    auto client = createLiteClient(InitialVersion::kOff);
    client->config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
    std::stringstream ss;
    ss << client->config;

    std::string sota_toml = (test_dir_.Path() / "sota.toml").string();
    Utils::writeFile(sota_toml, ss.str());

    auto new_target = createTarget();
    update(*client, getInitialTarget(), new_target);

    LOG_INFO << "Starting socket server at " << getSocketPath();
    aklite_ =
        std::make_unique<boost::process::child>(aklite_bin, "--config", sota_toml, "--socket-path", getSocketPath());
  }
  std::string getSocketPath() { return (test_dir_.Path() / "aklite.sock").string(); }

 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none) override {
    app_engine_mock_ = std::make_shared<NiceMock<MockAppEngine>>();
    return ClientTest::createLiteClient(app_engine_mock_, initial_version, apps);
  }

  std::shared_ptr<NiceMock<MockAppEngine>>& getAppEngine() { return app_engine_mock_; }

 private:
  std::shared_ptr<NiceMock<MockAppEngine>> app_engine_mock_;
  std::unique_ptr<boost::process::child> aklite_;
};

TEST_F(ApiServerTest, GetConfig) {
  startServer();
  HttpClient client(getSocketPath());
  auto resp = client.get("http://localhost/config", HttpInterface::kNoLimit);
  ASSERT_TRUE(resp.isOk());
  auto data = resp.getJson();
  ASSERT_EQ("true", data["telemetry"]["report_network"].asString());
}

TEST_F(ApiServerTest, GetCurrent) {
  startServer();
  HttpClient client(getSocketPath());
  auto resp = client.get("http://localhost/targets/current", HttpInterface::kNoLimit);
  ASSERT_TRUE(resp.isOk());
  auto data = resp.getJson();
  ASSERT_EQ("raspberrypi4-64-lmp-1", data["name"].asString());
  ASSERT_EQ(1, data["version"].asInt());
}

TEST_F(ApiServerTest, CheckIn) {
  startServer();
  HttpClient client(getSocketPath());
  auto resp = client.get("http://localhost/check_in", HttpInterface::kNoLimit);
  ASSERT_TRUE(resp.isOk());
  auto data = resp.getJson();
  ASSERT_EQ(1, data["targets"].size());

  createTarget();
  createTarget();
  resp = client.get("http://localhost/check_in", HttpInterface::kNoLimit);
  ASSERT_TRUE(resp.isOk());
  data = resp.getJson();
  ASSERT_EQ(3, data["targets"].size());
  ASSERT_EQ("raspberrypi4-64-lmp-1", data["targets"][0]["name"].asString());
  ASSERT_EQ("raspberrypi4-64-lmp-2", data["targets"][1]["name"].asString());
  ASSERT_EQ("raspberrypi4-64-lmp-3", data["targets"][2]["name"].asString());
  ASSERT_EQ(1, data["targets"][0]["version"].asInt());
  ASSERT_EQ(2, data["targets"][1]["version"].asInt());
  ASSERT_EQ(3, data["targets"][2]["version"].asInt());
}

TEST_F(ApiServerTest, GetRollback) {
  startServer();
  HttpClient client(getSocketPath());
  auto resp = client.get("http://localhost/targets/rollback/111", HttpInterface::kNoLimit);
  ASSERT_EQ(404, resp.http_status_code);
}

int main(int argc, char** argv) {
  if (argc != 4) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  // options passed as args in CMakeLists.txt
  fixtures::DeviceGatewayMock::RunCmd = argv[1];
  fixtures::SysRootFS::CreateCmd = argv[2];
  aklite_bin = argv[3];
  return RUN_ALL_TESTS();
}
