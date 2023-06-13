#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "cli/cli.h"
#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "fixtures/aklitetest.cc"

class CliClient : public AkliteTest {
 protected:
  std::shared_ptr<AkliteClient> createAkClient(InitialVersion initial_version = InitialVersion::kOn) {
    return std::make_shared<AkliteClient>(createLiteClient(initial_version));
  }

  TufTarget createTufTarget(AppEngine::App* app = nullptr, const std::string& hwid = "") {
    std::vector<AppEngine::App> apps;
    if (!app) {
      apps.emplace_back(registry.addApp(fixtures::ComposeApp::create("app-01")));
    } else {
      apps.push_back(*app);
    }
    return Target::toTufTarget(createTarget(&apps, hwid));
  }

  void reboot(std::shared_ptr<AkliteClient>& client, bool reset_bootupgrade_flag = true) {
    client.reset();
    boost::filesystem::remove(ClientTest::test_dir_.Path() / "need_reboot");
    if (reset_bootupgrade_flag) {
      boot_flag_mgr_->set("bootupgrade_available", "0");
    }
    client = std::make_shared<AkliteClient>(createLiteClient(InitialVersion::kOff, app_shortlist_, false));
  }

  void tweakConf(Config& conf) override {
    conf.pacman.ostree_server = ostree_server_uri_;
    conf.uptane.repo_server = tuf_repo_server_;
    conf.pacman.extra["ostree_update_block"] = "1";
  }

 protected:
  std::string tuf_repo_server_{device_gateway_.getTufRepoUri()};
  std::string ostree_server_uri_{device_gateway_.getOsTreeUri()};
};

TEST_P(CliClient, FullUpdate) {
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallNeedsReboot);
  reboot(akclient);
  ASSERT_TRUE(akclient->IsInstallationInProgress());
  ASSERT_EQ(akclient->GetPendingTarget(), target01);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::Ok);
}

TEST_P(CliClient, TufMetaDownloadFailure) {
  // make the TUF server URI invalid so the TUF metadata update fails
  tuf_repo_server_ = device_gateway_.getTufRepoUri() + "/foobar";
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::TufMetaPullFailure);
}

TEST_P(CliClient, TufTargetNotFoundInvalidHardwareId) {
  auto akclient{createAkClient()};
  auto target01 = createTufTarget(nullptr, "foobar-hwid");

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::TufTargetNotFound);
}

TEST_P(CliClient, TufTargetNotFoundInvalidVersion) {
  auto akclient{createAkClient()};
  auto target01 = createTufTarget(nullptr, "foobar-hwid");

  ASSERT_EQ(cli::Install(*akclient, 100), cli::StatusCode::TufTargetNotFound);
}

TEST_P(CliClient, OstreeDownloadFailure) {
  // Set invalid ostree server URI so the download fails
  ostree_server_uri_ = device_gateway_.getOsTreeUri() + "foobar";
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::DownloadFailure);
}

TEST_P(CliClient, AppDownloadFailure) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create(
      "app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate, "incorrect-compose-file.yml"));
  auto akclient{createAkClient()};
  const auto target01{createTufTarget(&app01)};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::DownloadFailure);
}

TEST_P(CliClient, UpdateIfBootFwUpdateRequiresReboot) {
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  // make the client think that there is pending boot fw update that requires reboot to be confirmed
  boot_flag_mgr_->set("bootupgrade_available");
  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallNeedsRebootForBootFw);
  // make sure that the istallation hasn't happened
  ASSERT_FALSE(akclient->IsInstallationInProgress());
  reboot(akclient);
  // make sure the client can install a target after the boot fw update confirmation (reboot)
  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallNeedsReboot);
  ASSERT_TRUE(akclient->IsInstallationInProgress());
  ASSERT_EQ(akclient->GetPendingTarget(), target01);
  // reboot a device and emulate the complex boot fw update that requires additional reboot after successful ostree and
  // App update
  reboot(akclient, false);
  ASSERT_TRUE(akclient->IsInstallationInProgress());
  ASSERT_EQ(akclient->GetPendingTarget(), target01);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::OkNeedsRebootForBootFw);
}

INSTANTIATE_TEST_SUITE_P(MultiEngine, CliClient, ::testing::Values("RestorableAppEngine"));

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
