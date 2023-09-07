// test fixtures, just include CC files
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/dockerregistry.cc"
#include "docker/restorableappengine.h"


namespace fixtures {


class AppEngineTest : virtual public ::testing::Test {
 protected:
  AppEngineTest() : registry{test_dir_.Path() / "registry"}, daemon_{test_dir_.Path() / "daemon"},
                    available_storage_space_{1024*1024 /* by default 2MB free space out of 4MB of storage capacity*/} {}

  void SetUp() override {
    auto env{boost::this_process::environment()};
    env.set("DOCKER_HOST", daemon_.getUnixSocket());
    compose_cmd =
        boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";

    apps_root_dir = test_dir_.Path() / "compose-apps";
    registry_client_ = std::make_shared<Docker::RegistryClient>(registry.getClient(), registry.authURL(), registry.getClientFactory());
    docker_client_ = std::make_shared<Docker::DockerClient>(daemon_.getClient());
  }

  void setAvailableStorageSpace(const boost::uintmax_t& space_size) {
    available_storage_space_ = space_size/this->watermark_;
  }
  void setAvailableStorageSpaceWithoutWatermark(const boost::uintmax_t& space_size) {
    available_storage_space_ = space_size;
  }

  virtual Docker::RestorableAppEngine::StorageSpaceFunc getTestStorageSpaceFunc() {
    return [this](const boost::filesystem::path& path) {
      // this->available_storage_space_ * this->watermark_ is available in the new math
      // let's assume that storage size is twice as the amount of free storage for sake of simplicity
      auto avail{this->available_storage_space_ * this->watermark_};
      return storage::Volume::UsageInfo{
          .path = path.string(),
          .size = {this->available_storage_space_ * 2, 100},
          .free = {this->available_storage_space_, 50},
          .reserved = {this->available_storage_space_ - avail, 50 * (1 - this->watermark_)},
          .reserved_by = "pacman:storage_watermark",
          .available = {avail, 50 * this->watermark_},
      };
    };
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
  boost::uintmax_t available_storage_space_;
  float watermark_{0.8};

 private:
  std::shared_ptr<HttpInterface> http_client_;
};


} // namespace fixtures
