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
  RestorableAppEngineTest() : AppEngineTest(), skopeo_store_root_{test_dir_ / "apps-store"} {}
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();

    app_engine = std::make_shared<Docker::RestorableAppEngine>(skopeo_store_root_, apps_root_dir, registry_client_,
                                                               docker_client_, registry.getSkopeoClient(),
                                                               daemon_.getUrl(), compose_cmd);
  }

  void removeAppManifest(const AppEngine::App& app) {
    const Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
    const auto manifest_file{skopeo_store_root_ / "apps" / uri.app / uri.digest.hash() / Docker::Manifest::Filename};
    boost::filesystem::remove(manifest_file);
  }

  void damageAppManifest(const AppEngine::App& app) {
    const Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
    const auto manifest_file{skopeo_store_root_ / "apps" / uri.app / uri.digest.hash() / Docker::Manifest::Filename};
    Json::Value manifest_json{Utils::parseJSONFile(manifest_file)};
    manifest_json["layers"][0]["digest"] = "sha256:4a7c02f3267e2b92c0d1d78432acf611906b70964df8e27ab7d4c6f835efdqqq";
    Utils::writeFile(manifest_file, manifest_json);
  }

  void damageAppArchive(const AppEngine::App& app) {
    const Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
    const auto app_dir{skopeo_store_root_ / "apps" / uri.app / uri.digest.hash()};
    const auto manifest_file{app_dir / Docker::Manifest::Filename};
    Docker::Manifest manifest{Utils::parseJSONFile(manifest_file)};
    const auto archive_full_path{
        app_dir / (Docker::HashedDigest(manifest.archiveDigest()).hash() + Docker::Manifest::ArchiveExt)};
    Utils::writeFile(archive_full_path, std::string("foo bar"));
  }

 private:
  const boost::filesystem::path skopeo_store_root_;
};

TEST_F(RestorableAppEngineTest, InitDeinit) {}

TEST_F(RestorableAppEngineTest, Fetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndInstall) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-02"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-03"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

/**
 * @brief Make sure that App content is fetched once, provided that an initial fetch was successful
 */
TEST_F(RestorableAppEngineTest, FetchFetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-031"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
  ASSERT_EQ(1, registry.getAppManifestPullNumb(app.uri));

  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
  ASSERT_EQ(1, registry.getAppManifestPullNumb(app.uri));

  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchInstallAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-04"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchRunAndUpdate) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-05"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));

  // update App, image URL has changed
  auto updated_app = registry.addApp(fixtures::ComposeApp::create("app-05", "service-01", "image-02"));
  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_TRUE(app_engine->isFetched(updated_app));
  ASSERT_FALSE(app_engine->isRunning(updated_app));

  // run updated App
  ASSERT_TRUE(app_engine->run(updated_app));
  ASSERT_TRUE(app_engine->isRunning(updated_app));
}

TEST_F(RestorableAppEngineTest, FetchRunCompare) {
  const auto app{fixtures::ComposeApp::create("app-06", "service-02", "image-02")};
  auto updated_app = registry.addApp(app);

  // the format is defined by DockerClient.cc
  boost::format format("App(%s) Service(%s ");
  std::string id = boost::str(format % "app-06" % "service-02");

  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_TRUE(app_engine->isFetched(updated_app));
  ASSERT_FALSE(app_engine->isRunning(updated_app));
  ASSERT_FALSE(app_engine->getRunningAppsInfo().isMember("app-06"));

  // run updated App
  ASSERT_TRUE(app_engine->run(updated_app));
  ASSERT_TRUE(app_engine->isRunning(updated_app));

  Json::Value apps_info = app_engine->getRunningAppsInfo();
  ASSERT_TRUE(apps_info.isMember("app-06"));
  ASSERT_TRUE(apps_info["app-06"]["services"].isMember("service-02"));
  ASSERT_EQ(apps_info["app-06"]["services"]["service-02"]["image"].asString(), app->image().uri());
}

/**
 * @brief Make sure that App content is re-fetched if manifest wasn't fetched properly
 */
TEST_F(RestorableAppEngineTest, ManifestFetchFailureAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-004"));

  {
    // manifest wasn't stored for some reason or removed somehow
    ASSERT_TRUE(app_engine->fetch(app));
    removeAppManifest(app);
    ASSERT_FALSE(app_engine->isFetched(app));
    ASSERT_EQ(1, registry.getAppManifestPullNumb(app.uri));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_FALSE(app_engine->isRunning(app));
    ASSERT_EQ(2, registry.getAppManifestPullNumb(app.uri));
  }

  {
    // manifest was damaged
    damageAppManifest(app);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_FALSE(app_engine->isRunning(app));
    ASSERT_EQ(3, registry.getAppManifestPullNumb(app.uri));
  }

  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

/**
 * @brief Make sure that App content is re-fetched if App archive wasn't fetched properly
 */
TEST_F(RestorableAppEngineTest, AppArchiveFetchFailureAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-004"));

  ASSERT_TRUE(app_engine->fetch(app));
  damageAppArchive(app);
  ASSERT_FALSE(app_engine->isFetched(app));
  ASSERT_EQ(1, registry.getAppManifestPullNumb(app.uri));

  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
  ASSERT_EQ(2, registry.getAppManifestPullNumb(app.uri));

  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
