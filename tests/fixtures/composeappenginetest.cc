// test fixtures, just include CC files
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/dockerregistry.cc"


namespace fixtures {


class AppEngineTest : virtual public ::testing::Test {
 protected:
  AppEngineTest() : registry{test_dir_.Path() / "registry"}, daemon_{test_dir_.Path() / "daemon"} {}

  void SetUp() override {
    auto env{boost::this_process::environment()};
    env.set("DOCKER_HOST", daemon_.getUrl());
    compose_cmd =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";

    apps_root_dir = test_dir_.Path() / "compose-apps";
    registry_client_ = std::make_shared<Docker::RegistryClient>(registry.getClient(), registry.authURL(), registry.getClientFactory());
    docker_client_ = std::make_shared<Docker::DockerClient>(daemon_.getClient());
  }

 protected:
  TemporaryDirectory test_dir_;
  fixtures::DockerRegistry registry;
  fixtures::DockerDaemon daemon_;
  Docker::RegistryClient::Ptr registry_client_;
  Docker::DockerClient::Ptr docker_client_;

  std::string compose_cmd;
  boost::filesystem::path apps_root_dir;
  std::shared_ptr<AppEngine> app_engine;

 private:
  std::shared_ptr<HttpInterface> http_client_;
};


} // namespace fixtures
