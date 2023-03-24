#include <gtest/gtest.h>
#include <iostream>
#include <unordered_set>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"
#include "test_utils.h"

#include "docker/composeappengine.h"
#include "logging/logging.h"

#include "fixtures/composeappenginetest.cc"

class ComposeAppEngineTest : public fixtures::AppEngineTest {
 protected:
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();
    app_engine =
        std::make_shared<Docker::ComposeAppEngine>(apps_root_dir, compose_cmd, docker_client_, registry_client_);
  }
};

TEST_F(ComposeAppEngineTest, Fetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  // TODO: AppEngine API doesn't provide mean(s) to check if App is fetched
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineTest, FetchIfNoAuth) {
  registry.setNoAuth(true);
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  registry.setNoAuth(false);
}

TEST_F(ComposeAppEngineTest, FetchIfInvalidAuth) {
  registry.setAuthFunc([](const std::string& url) {
    // no opening `"` after `=`
    return "bearer realm = https://hub-auth.foundries.io/token-auth/"
           "\",service=\"registry\",scope=\"repository:msul-dev01/simpleapp:pull\"";
  });
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  const auto res{app_engine->fetch(app)};
  ASSERT_FALSE(res);
  ASSERT_TRUE(boost::starts_with(res.err, "Invalid value of Bearer auth parameters"));
  registry.setAuthFunc(nullptr);
}

TEST_F(ComposeAppEngineTest, FetchAndInstall) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_TRUE(app_engine->getInstalledApps() & app);
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineTest, FetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->getInstalledApps() & app);
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineTest, FetchInstallAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_TRUE(app_engine->getInstalledApps() & app);
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(ComposeAppEngineTest, FetchRunAndUpdate) {
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
  ASSERT_TRUE(app_engine->getInstalledApps() & updated_app);
}

TEST_F(ComposeAppEngineTest, FetchRunCompare) {
  const auto app{fixtures::ComposeApp::create("app-02", "service-02", "image-02")};
  auto updated_app = registry.addApp(app);

  // the format is defined by DockerClient.cc
  boost::format format("App(%s) Service(%s ");
  std::string id = boost::str(format % "app-02" % "service-02");

  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_FALSE(app_engine->isRunning(updated_app));
  ASSERT_FALSE(app_engine->getRunningAppsInfo().isMember("app-02"));

  // run updated App
  ASSERT_TRUE(app_engine->run(updated_app));
  ASSERT_TRUE(app_engine->isRunning(updated_app));

  const auto installed_apps{app_engine->getInstalledApps()};
  ASSERT_EQ(installed_apps[0], updated_app);

  Json::Value apps_info = app_engine->getRunningAppsInfo();
  ASSERT_TRUE(apps_info.isMember("app-02"));
  ASSERT_EQ(apps_info["app-02"]["services"][0]["name"], "service-02");
  ASSERT_EQ(apps_info["app-02"]["services"][0]["image"].asString(), app->image().uri());
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
