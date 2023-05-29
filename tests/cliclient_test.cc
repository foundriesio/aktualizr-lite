#include <gmock/gmock.h>
#include <gtest/gtest.h>
#include <boost/format.hpp>
#include <boost/process.hpp>

#include <aktualizr-lite/api.h>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"

#include "fixtures/aklitetest.cc"

class CliClient : public AkliteTest {
 protected:
  std::shared_ptr<AkliteClient> createAkClient() { return std::make_shared<AkliteClient>(createLiteClient()); }
  void reboot(std::shared_ptr<AkliteClient>& client) {
    client.reset();
    boost::filesystem::remove(ClientTest::test_dir_.Path() / "need_reboot");
    client = std::make_shared<AkliteClient>(createLiteClient(InitialVersion::kOff, app_shortlist_, false));
  }
  void tweakConf(Config& conf) override { conf.pacman.ostree_server = ostree_server_uri_; }

 protected:
  std::string ostree_server_uri_{device_gateway_.getOsTreeUri()};
};

TEST_P(CliClient, AppUpdate) {
  auto akclient{createAkClient()};

  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto target01 = createAppTarget({app01});

  ASSERT_EQ(cli::Install(*akclient, std::stoi(target01.custom_version())), cli::ExitCode::Ok);
  ASSERT_EQ(akclient->GetCurrent().Name(), target01.filename());
}

TEST_P(CliClient, FullUpdate) {
  auto akclient{createAkClient()};

  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  std::vector<AppEngine::App> apps{app01};
  auto target01 = createTarget(&apps);

  ASSERT_EQ(cli::Install(*akclient, std::stoi(target01.custom_version())), cli::ExitCode::InstallNeedsReboot);
  reboot(akclient);
  ASSERT_EQ(akclient->GetPendingTarget().Name(), target01.filename());
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::ExitCode::Ok);
  ASSERT_EQ(akclient->GetCurrent().Name(), target01.filename());
  ASSERT_TRUE(akclient->GetPendingTarget().IsUnknown());
}

TEST_P(CliClient, OstreeDownloadFailure) {
  // Set invalid ostree server URI so the download fails
  ostree_server_uri_ = device_gateway_.getOsTreeUri() + "foobar";
  auto akclient{createAkClient()};
  auto target01 = createTarget();
  ASSERT_EQ(cli::Install(*akclient, std::stoi(target01.custom_version())), cli::ExitCode::DownloadFailure);
}

TEST_P(CliClient, AppDownloadFailure) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create(
      "app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate, "incorrect-compose-file.yml"));
  auto akclient{createAkClient()};
  auto target01 = createAppTarget({app01});
  ASSERT_EQ(cli::Install(*akclient, std::stoi(target01.custom_version())), cli::ExitCode::DownloadFailure);
}

INSTANTIATE_TEST_SUITE_P(MultiEngine, CliClient, ::testing::Values("RestorableAppEngine", "ComposeAppEngine"));

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  // options passed as args in CMakeLists.txt
  fixtures::DeviceGatewayMock::RunCmd = argv[1];
  fixtures::SysRootFS::CreateCmd = argv[2];
  return RUN_ALL_TESTS();
}
