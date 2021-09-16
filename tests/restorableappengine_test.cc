#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"
#include "logging/logging.h"
#include "test_utils.h"

#include "docker/composeappengine.h"
#include "docker/restorableappengine.h"

#include "fixtures/composeappenginetest.cc"

class RestorableAppEngineTest : public fixtures::AppEngineTest {
 protected:
  RestorableAppEngineTest() : fixtures::AppEngineTest(), apps_store_root_dir_{test_dir_.Path() / "apps-store"} {}
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();

    app_engine = std::make_shared<Docker::RestorableAppEngine>(apps_store_root_dir_, apps_root_dir, registry_client_,
                                                               registry.getSkopeoClient(), daemon_.getUrl());
  }

 protected:
  boost::filesystem::path apps_store_root_dir_;
};

TEST_F(RestorableAppEngineTest, InitDeinit) {}

TEST_F(RestorableAppEngineTest, Fetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
}

TEST_F(RestorableAppEngineTest, FetchAndInstall) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-02"));
  ASSERT_TRUE(app_engine->fetch(app));
  // TODO: AppEngine API doesn't provide mean(s) to check if App is installed
  ASSERT_TRUE(app_engine->install(app));
  // ASSERT_FALSE(app_engine->isRunning(app));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
