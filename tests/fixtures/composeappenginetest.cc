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
    return true;
  }
  bool copyAppImageToDockerStore(const AppEngine::App& app, const std::string& image) const override {
    return true;
  }

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
