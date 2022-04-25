#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>
#include <limits>

#include "crypto/crypto.h"
#include "logging/logging.h"
#include "test_utils.h"

#include "docker/composeappengine.h"
#include "docker/composeinfo.h"
#include "docker/restorableappengine.h"

#include "fixtures/composeappenginetest.cc"

class RestorableAppEngineTest : public fixtures::AppEngineTest {
 protected:
  RestorableAppEngineTest()
      : AppEngineTest(),
        skopeo_store_root_{test_dir_ / "apps-store"},
        available_storage_space_{std::numeric_limits<boost::uintmax_t>::max()} {}
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();

    app_engine = std::make_shared<Docker::RestorableAppEngine>(
        skopeo_store_root_, apps_root_dir, daemon_.dataRoot(), registry_client_, docker_client_,
        registry.getSkopeoClient(), daemon_.getUrl(), compose_cmd, [this](const boost::filesystem::path& path) {
          return std::tuple<boost::uintmax_t, boost::uintmax_t>{this->available_storage_space_,
                                                                (this->watermark_ * this->available_storage_space_)};
        });
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

  void setAvailableStorageSpace(const boost::uintmax_t& space_size) {
    available_storage_space_ = space_size / watermark_;
  }
  void setAvailableStorageSpaceWithoutWatermark(const boost::uintmax_t& space_size) {
    available_storage_space_ = space_size;
  }
  const boost::filesystem::path& storeRoot() const { return skopeo_store_root_; }

 protected:
  const boost::filesystem::path skopeo_store_root_;
  boost::uintmax_t available_storage_space_;
  float watermark_{0.8};
};

class RestorableAppEngineTestParameterized : public RestorableAppEngineTest,
                                             public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();
    const auto docker_data_root_path{GetParam()};

    app_engine = std::make_shared<Docker::RestorableAppEngine>(
        skopeo_store_root_, apps_root_dir, docker_data_root_path.empty() ? daemon_.dataRoot() : docker_data_root_path,
        registry_client_, docker_client_, registry.getSkopeoClient(), daemon_.getUrl(), compose_cmd,
        [this](const boost::filesystem::path& path) {
          return std::tuple<boost::uintmax_t, boost::uintmax_t>{this->available_storage_space_,
                                                                (this->watermark_ * this->available_storage_space_)};
        });
  }
};

TEST_F(RestorableAppEngineTest, InitDeinit) {}

TEST_F(RestorableAppEngineTest, Fetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchCheckAndRefetch) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_FALSE(app_engine->isRunning(app));

  const Docker::Uri uri{Docker::Uri::parseUri(app.uri)};
  const auto app_dir{storeRoot() / "apps" / uri.app / uri.digest.hash()};

  {
    // remove App dir
    boost::filesystem::remove_all(app_dir);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // remove App manifest
    boost::filesystem::remove(app_dir / Docker::Manifest::Filename);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // alter App manifest
    Utils::writeFile(app_dir / Docker::Manifest::Filename, std::string("foo bar"));
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  const Docker::Manifest manifest{Utils::parseJSONFile(app_dir / Docker::Manifest::Filename)};
  {
    // remove App archive/blob
    boost::filesystem::remove(app_dir /
                              (Docker::HashedDigest(manifest.archiveDigest()).hash() + Docker::Manifest::ArchiveExt));
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // alter App archive/blob
    Utils::writeFile(app_dir / (Docker::HashedDigest(manifest.archiveDigest()).hash() + Docker::Manifest::ArchiveExt),
                     std::string("foo bar"));
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // remove App images dir
    boost::filesystem::remove_all(app_dir / "images");
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }

  const auto compose_file{app_dir / Docker::RestorableAppEngine::ComposeFile};
  Docker::ComposeInfo compose{compose_file.string()};
  const auto image = compose.getImage(compose.getServices()[0]);
  const Docker::Uri image_uri{Docker::Uri::parseUri(image, false)};
  const auto image_root{app_dir / "images" / image_uri.registryHostname / image_uri.repo / image_uri.digest.hash()};
  const auto index_manifest{image_root / "index.json"};

  {
    // remove App image dir
    boost::filesystem::remove_all(image_root);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // remove App image index manifest
    boost::filesystem::remove(index_manifest);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }

  const auto manifest_desc{Utils::parseJSONFile(index_manifest)};
  Docker::HashedDigest manifest_digest{manifest_desc["manifests"][0]["digest"].asString()};
  const auto blob_dir{storeRoot() / "blobs" / "sha256"};
  const auto manifest_file{blob_dir / manifest_digest.hash()};

  {
    // remove App blobs dir
    boost::filesystem::remove_all(blob_dir);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // remove App image manifest
    boost::filesystem::remove(manifest_file);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // alter App image manifest
    Utils::writeFile(manifest_file, std::string("foo bar"));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  const auto image_manifest{Utils::parseJSONFile(manifest_file)};
  const auto blob_digest{Docker::HashedDigest(image_manifest["layers"][0]["digest"].asString())};
  {
    // remove App image blob
    boost::filesystem::remove(blob_dir / blob_digest.hash());
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
  {
    // alter App image blob
    Utils::writeFile(blob_dir / blob_digest.hash(), std::string("foo bar"));
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
  }
}

TEST_F(RestorableAppEngineTest, FetchAndInstall) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-02"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_FALSE(app_engine->isRunning(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-03"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndCheckSizeNoManifest) {
  // If a manifest with a layer list is not present an update should succeed anyway, so
  // the "size-aware" aklite can download Targets created before the "size-aware" compose-publish is deployed.
  auto app = registry.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", Json::Value()));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
}

TEST_F(RestorableAppEngineTest, CheckStorageWatermarkLimits) {
  EXPECT_THROW(Docker::RestorableAppEngine::GetDefStorageSpaceFunc(Docker::RestorableAppEngine::HighWatermarkLimit + 1),
               std::invalid_argument);
  EXPECT_THROW(Docker::RestorableAppEngine::GetDefStorageSpaceFunc(Docker::RestorableAppEngine::LowWatermarkLimit - 1),
               std::invalid_argument);
}

TEST_F(RestorableAppEngineTest, FetchAndCheckSizeInsufficientSpace) {
  setAvailableStorageSpace(1024);
  auto app = registry.addApp(fixtures::ComposeApp::create("app-01"));
  ASSERT_TRUE(app_engine->fetch(app).noSpace());
  ASSERT_FALSE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndCheckSizeInsufficientSpaceIfWatermark) {
  const auto layer_size{1024};
  Json::Value layers;
  layers["layers"][0]["digest"] =
      "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
  layers["layers"][0]["size"] = layer_size;

  {
    const auto compose_app{fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers)};
    setAvailableStorageSpace(6144);
    auto app = registry.addApp(compose_app);
    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
  }
  {
    const auto compose_app{fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers)};
    setAvailableStorageSpaceWithoutWatermark(6144);
    auto app = registry.addApp(compose_app);
    ASSERT_TRUE(app_engine->fetch(app).noSpace());
    ASSERT_FALSE(app_engine->isFetched(app));
  }
}

TEST_P(RestorableAppEngineTestParameterized, FetchAndCheckSizeInsufficientSpace) {
  const auto layer_size{1024};
  Json::Value layers;
  layers["layers"][0]["digest"] =
      "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
  layers["layers"][0]["size"] = layer_size;

  const auto compose_app{fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers)};
  // storage size sufficient to accomodate a layer in the skopeo store
  // but not sufficient to accomodate an uncompressed layer in the docker data root (store)
  setAvailableStorageSpace(layer_size * 1.5);
  auto app = registry.addApp(compose_app);
  ASSERT_TRUE(app_engine->fetch(app).noSpace());
  ASSERT_FALSE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

// Run FetchAndCheckSizeInsufficientSpace test for two use-cases:
// 1. The skopeo and docker store are located on the same volume.
// 2. The skopeo and docker store are located on different volumes.
INSTANTIATE_TEST_SUITE_P(CheckSizeTests, RestorableAppEngineTestParameterized,
                         ::testing::Values("", "/var/non-existing-dir/docker"));

TEST_F(RestorableAppEngineTest, FetchAndCheckSizeOverflowLayerSize) {
  // generate a list of layers an overall size of which exceeds std::uint64_t/std::size_t
  // layer sizes must be correct, i.e. int64, so we need 2 layers with int64::max + 2
  Json::Value layers;
  for (int ii = 0; ii < 3; ++ii) {
    layers["layers"][ii]["digest"] =
        "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
    layers["layers"][ii]["size"] = std::numeric_limits<std::int64_t>::max();
  }

  auto app = registry.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers));
  ASSERT_FALSE(app_engine->fetch(app));
  ASSERT_FALSE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndCheckSizeInvalidLayersManifestSize) {
  Json::Value layers;
  layers["layers"][0]["digest"] =
      "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
  layers["layers"][0]["size"] = 1024;

  auto app = registry.addApp(fixtures::ComposeApp::createAppWithCustomeLayers(
      "app-01", layers, (Utils::jsonToCanonicalStr(layers).size() - 1)));
  ASSERT_FALSE(app_engine->fetch(app));
  ASSERT_FALSE(app_engine->isFetched(app));
  ASSERT_FALSE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchAndCheckSizeInvalidLayerSize) {
  {
    // layer sizes must be int64, we set it to uint64::max to check how the given negative case is handled
    Json::Value layers;
    layers["layers"][0]["digest"] =
        "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
    layers["layers"][0]["size"] = std::numeric_limits<std::uint64_t>::max();

    auto app = registry.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers));
    ASSERT_FALSE(app_engine->fetch(app));
    ASSERT_FALSE(app_engine->isFetched(app));
    ASSERT_FALSE(app_engine->isRunning(app));
  }
  {
    // layer size cannot be negative
    Json::Value layers;
    layers["layers"][0]["digest"] =
        "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
    layers["layers"][0]["size"] = -1024;

    auto app = registry.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers));
    ASSERT_FALSE(app_engine->fetch(app));
    ASSERT_FALSE(app_engine->isFetched(app));
    ASSERT_FALSE(app_engine->isRunning(app));
  }
}

/**
 * @brief Make sure that App content is fetched once, provided that an initial fetch was successful
 */
TEST_F(RestorableAppEngineTest, FetchFetchAndRun) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-031"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->verify(app));
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
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_TRUE(app_engine->install(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, FetchRunAndUpdate) {
  auto app = registry.addApp(fixtures::ComposeApp::create("app-05"));
  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));

  // update App, image URL has changed
  auto updated_app = registry.addApp(fixtures::ComposeApp::create("app-05", "service-01", "image-02"));
  ASSERT_TRUE(app_engine->fetch(updated_app));
  ASSERT_TRUE(app_engine->isFetched(updated_app));
  ASSERT_TRUE(app_engine->verify(updated_app));
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
  ASSERT_TRUE(app_engine->verify(updated_app));
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
    ASSERT_TRUE(app_engine->verify(app));
    ASSERT_FALSE(app_engine->isRunning(app));
    ASSERT_EQ(2, registry.getAppManifestPullNumb(app.uri));
  }

  {
    // manifest was damaged
    damageAppManifest(app);
    ASSERT_FALSE(app_engine->isFetched(app));

    ASSERT_TRUE(app_engine->fetch(app));
    ASSERT_TRUE(app_engine->isFetched(app));
    ASSERT_TRUE(app_engine->verify(app));
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
  ASSERT_TRUE(app_engine->verify(app));
  ASSERT_FALSE(app_engine->isRunning(app));
  ASSERT_EQ(2, registry.getAppManifestPullNumb(app.uri));

  ASSERT_TRUE(app_engine->run(app));
  ASSERT_TRUE(app_engine->isRunning(app));
}

TEST_F(RestorableAppEngineTest, VerifyFailure) {
  // invalid service definition, `ports` value must be integer
  const std::string AppInvalidServiceTemplate = R"(
      %s:
        image: %s
        ports:
          - foo:bar)";

  auto app =
      registry.addApp(fixtures::ComposeApp::create("app-005", "service-01", "image-01", AppInvalidServiceTemplate));

  ASSERT_TRUE(app_engine->fetch(app));
  ASSERT_TRUE(app_engine->isFetched(app));
  ASSERT_EQ(1, registry.getAppManifestPullNumb(app.uri));
  ASSERT_FALSE(app_engine->verify(app));
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
