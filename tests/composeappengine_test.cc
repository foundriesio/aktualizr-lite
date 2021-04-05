#include <gtest/gtest.h>
#include <iostream>
#include <unordered_set>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"

#include "docker/composeappengine.h"
#include "logging/logging.h"

// test fixtures, just include CC files
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/dockerregistry.cc"

class ComposeAppEngineeTest : public ::testing::Test {
 protected:
  ComposeAppEngineeTest() : registry{test_dir_.Path() / "registry"}, daemon_{test_dir_.Path() / "daemon"} {}

  void SetUp() override {
    auto compose_cmd =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";
    app_engine = std::make_shared<Docker::ComposeAppEngine>(
        test_dir_.Path() / "compose-apps", compose_cmd, std::make_shared<Docker::DockerClient>(daemon_.getClient()),
        std::make_shared<Docker::RegistryClient>("https://ota-lite.foundries.io:8443/", registry.getClient(),
                                                 registry.getClientFactory()));
  }

 protected:
  TemporaryDirectory test_dir_;
  fixtures::DockerRegistry registry;
  fixtures::DockerDaemon daemon_;

  std::shared_ptr<AppEngine> app_engine;

 private:
  std::shared_ptr<HttpInterface> http_client_;
};

TEST_F(ComposeAppEngineeTest, Fetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  // TODO: AppEngine API doesn't provide mean(s) to check if App is fetched
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineeTest, FetchAndInstall) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  // TODO: AppEngine API doesn't provide mean(s) to check if App is installed
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineeTest, FetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineeTest, FetchInstallAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineeTest, FetchRunAndUpdate) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));

  // update App, image URL has changed
  auto updated_app = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_FALSE(app_engine->isRunning(updated_app));

  // run updated App
  ASSERT_TRUE(app_engine->run(updated_app));
  ASSERT_TRUE(app_engine->isRunning(updated_app));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
