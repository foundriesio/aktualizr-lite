// test fixtures, just include CC files
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/dockerregistry.cc"

namespace fixtures {


class AppEngineTest : virtual public ::testing::Test {
 protected:
  AppEngineTest() : registry{test_dir_.Path() / "registry"}, daemon_{test_dir_.Path() / "daemon"} {}

  void SetUp() override {
    auto compose_cmd =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";

    apps_root_dir = test_dir_.Path() / "compose-apps";
    app_engine = std::make_shared<Docker::ComposeAppEngine>(apps_root_dir, compose_cmd, std::make_shared<Docker::DockerClient>(daemon_.getClient()),
                                                            std::make_shared<Docker::RegistryClient>(registry.getClient(), registry.authURL(), registry.getClientFactory()));
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
