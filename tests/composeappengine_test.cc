#include <gtest/gtest.h>
#include <iostream>
#include <unordered_set>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"

#include "docker/composeappengine.h"
#include "logging/logging.h"

#include "fixtures/composeappenginetest.cc"

class ComposeAppEngineTest : public fixtures::AppEngineTest {};

TEST_F(ComposeAppEngineTest, Fetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isInstalled(app));
  ASSERT_FALSE(app_engine->isStarted(app));
}

TEST_F(ComposeAppEngineTest, FetchAndInstall) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isInstalled(app));
  ASSERT_FALSE(app_engine->isStarted(app));
}

TEST_F(ComposeAppEngineTest, FetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isInstalled(app));
  ASSERT_TRUE(app_engine->isStarted(app));
}

TEST_F(ComposeAppEngineTest, FetchInstallAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isInstalled(app));
  ASSERT_TRUE(app_engine->isStarted(app));
}

TEST_F(ComposeAppEngineTest, FetchRunAndUpdate) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isInstalled(app));
  ASSERT_TRUE(app_engine->isStarted(app));

  // update App, image URL has changed
  auto updated_app = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_TRUE(app_engine->isInstalled(updated_app));
  ASSERT_FALSE(app_engine->isStarted(updated_app));

  // run updated App
  ASSERT_TRUE(app_engine->run(updated_app));
  ASSERT_TRUE(app_engine->isInstalled(updated_app));
  ASSERT_TRUE(app_engine->isStarted(updated_app));
}

TEST_F(ComposeAppEngineTest, FetchRunCompare) {
  auto updated_app = registry.addApp(fixtures::ComposeApp::create("app-02", "service-02", "image-02"));

  // the format is defined by DockerClient.cc
  boost::format format("App(%s) Service(%s ");
  std::string id = boost::str(format % "app-02" % "service-02");

  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_TRUE(app_engine->isInstalled(updated_app));
  ASSERT_FALSE(app_engine->isStarted(updated_app));
  ASSERT_FALSE(boost::icontains(app_engine->runningApps(), id));

  // run updated App
  ASSERT_TRUE(app_engine->run(updated_app));
  ASSERT_TRUE(app_engine->isInstalled(updated_app));
  ASSERT_TRUE(app_engine->isStarted(updated_app));
  ASSERT_TRUE(boost::icontains(app_engine->runningApps(), id));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
