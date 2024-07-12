#include "fixtures/composeappenginetest.cc"
#include "fixtures/liteclienttest.cc"

class AkliteTest : public fixtures::ClientTest,
                   public fixtures::AppEngineTest,
                   public ::testing::WithParamInterface<std::string> {
 protected:
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();

    const auto app_engine_type{GetParam()};

    if (app_engine_type == "ComposeAppEngine") {
      app_engine =
          std::make_shared<Docker::ComposeAppEngine>(apps_root_dir, compose_cmd, docker_client_, registry_client_);
    } else if (app_engine_type == "RestorableAppEngine") {
      app_engine = std::make_shared<Docker::RestorableAppEngine>(
          fixtures::ClientTest::test_dir_.Path() / "apps-store", apps_root_dir, daemon_.dataRoot(), registry_client_,
          docker_client_, registry.getSkopeoClient(), daemon_.getUrl(), compose_cmd, getTestStorageSpaceFunc());
    } else {
      throw std::invalid_argument("Unsupported AppEngine type: " + app_engine_type);
    }
  }

  std::shared_ptr<fixtures::LiteClientMock> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               bool finalize = true) override {
    const auto app_engine_type{GetParam()};

    if (app_engine_type == "ComposeAppEngine") {
      return ClientTest::createLiteClient(app_engine, initial_version, apps, apps_root_dir.string(), boost::none,
                                          create_containers_before_reboot_, finalize);
    } else if (app_engine_type == "RestorableAppEngine") {
      return ClientTest::createLiteClient(app_engine, initial_version, apps, apps_root_dir.string(),
                                          !!apps ? apps : std::vector<std::string>{""},
                                          create_containers_before_reboot_, finalize);
    } else {
      throw std::invalid_argument("Unsupported AppEngine type: " + app_engine_type);
    }
  }

  void setCreateContainersBeforeReboot(bool value) { create_containers_before_reboot_ = value; }
  void tweakConf(Config& conf) override {
    conf.pacman.extra["images_data_root"] = daemon_.dataRoot();
  };

 private:
  bool create_containers_before_reboot_{true};
};
