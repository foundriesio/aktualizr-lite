// test fixtures, just include CC files
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/dockerregistry.cc"
#include "docker/restorableappengine.h"


namespace fixtures {


class MockAppStore: public Docker::SkopeoAppStore {
 public:
  MockAppStore(boost::filesystem::path root, Docker::RegistryClient::Ptr registry_client):Docker::SkopeoAppStore("", root, registry_client) {}

  bool pullAppImage(const AppEngine::App& app, const std::string& uri, const std::string& auth) const override {

    // emulate `skopeo copy` command (from docker registry to OCI store
    const auto dst_path{getAppImageRoot(app, uri)};
    boost::filesystem::create_directories(dst_path);
    Utils::writeFile(dst_path / "index.json", ImageIndexTemplate);
    const auto blob_path{blobsRoot() / "sha256" / "4c5072c18f5fb3e5435acef3d1c61c10a6490b214967c47351cace9addc5ec42"};
    Utils::writeFile(blob_path, ImageManifestTemplate);

    return true;
  }

  bool copyAppImageToDockerStore(const AppEngine::App& app, const std::string& image) const override {
    return true;
  }

 private:
  const std::string ImageIndexTemplate = R"(
  {
    "schemaVersion": 2,
    "manifests": [
      {
        "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
        "digest": "sha256:4c5072c18f5fb3e5435acef3d1c61c10a6490b214967c47351cace9addc5ec42",
        "size": 1575
      }
    ]
  }
)";

  const std::string ImageManifestTemplate = R"(
{
   "mediaType": "application/vnd.docker.distribution.manifest.v2+json",
   "schemaVersion": 2,
   "config": {
      "mediaType": "application/vnd.docker.container.image.v1+json",
      "digest": "sha256:b0a2e960f732fdea26ccfaea95db03d18d4a32ca4d74f6ea1d3e2e53f8865424",
      "size": 5692
   },
   "layers": [
      {
         "mediaType": "application/vnd.docker.image.rootfs.diff.tar.gzip",
         "digest": "sha256:48ecbb6b270eb481cb6df2a5b0332de294ec729e1968e92d725f1329637ce01b",
         "size": 2107173
      }
    ]
}
)";

};


class AppEngineTest : virtual public ::testing::Test {
 protected:
  AppEngineTest() : registry{test_dir_.Path() / "registry"}, daemon_{test_dir_.Path() / "daemon"} {}

  void SetUp() override {
    auto compose_cmd =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";

    apps_root_dir = test_dir_.Path() / "compose-apps";
    const auto apps_store_root_dir{test_dir_.Path() / "reset-apps"};
    auto registry_client{std::make_shared<Docker::RegistryClient>(registry.getClient(), registry.authURL(), registry.getClientFactory())};
    app_engine = std::make_shared<Docker::RestorableAppEngine>(apps_root_dir, compose_cmd, std::make_shared<Docker::DockerClient>(daemon_.getClient()), registry_client,
                                                               std::make_shared<MockAppStore>(apps_store_root_dir, registry_client));
  }

 protected:
  TemporaryDirectory test_dir_;
  fixtures::DockerRegistry registry;
  fixtures::DockerDaemon daemon_;

  boost::filesystem::path apps_root_dir;
  std::shared_ptr<AppEngine> app_engine;

 private:
  std::shared_ptr<HttpInterface> http_client_;
};


} // namespace fixtures
