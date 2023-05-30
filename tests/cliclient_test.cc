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
  std::string ostree_server_uri_{device_gateway_.getOsTreeUri()};
  std::string tuf_repo_server_{device_gateway_.getTufRepoUri()};
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

TEST_P(CliClient, TufMetaDownloadFailure) {
  tuf_repo_server_ = device_gateway_.getTufRepoUri() + "/foobar";
  auto akclient{createAkClient()};
  auto target01 = createTarget();
  ASSERT_EQ(cli::Install(*akclient, std::stoi(target01.custom_version())), cli::ExitCode::TufMetaPullFailure);
}

TEST_P(CliClient, TufTargetNotFoundInvalidHardwareId) {
  auto akclient{createAkClient()};
  auto target01 = createTarget(nullptr, "foobar-hwid");
  ASSERT_EQ(cli::Install(*akclient, std::stoi(target01.custom_version())), cli::ExitCode::TufTargetNotFound);
}

TEST_P(CliClient, TufTargetNotFoundInvalidVersion) {
  auto akclient{createAkClient()};
  auto target01 = createTarget();
  ASSERT_EQ(cli::Install(*akclient, 100), cli::ExitCode::TufTargetNotFound);
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

TEST_P(CliClient, UpdateIfBootFwUpdateIsNotConfirmedBefore) {
  auto akclient{createAkClient()};

  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  std::vector<AppEngine::App> apps{app01};
  auto target01 = Target::toTufTarget(createTarget(&apps));

  {
    boot_flag_mgr_->set("bootupgrade_available");
    ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::ExitCode::InstallNeedsRebootForBootFw);
  }
  reboot(akclient);

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::ExitCode::InstallNeedsReboot);
  reboot(akclient, false);
  ASSERT_EQ(akclient->GetPendingTarget(), target01);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::ExitCode::InstallNeedsRebootForBootFw);
  ASSERT_EQ(akclient->GetCurrent(), target01);
  ASSERT_TRUE(akclient->GetPendingTarget().IsUnknown());
  reboot(akclient);
  ASSERT_EQ(akclient->GetCurrent(), target01);
  ASSERT_TRUE(akclient->GetPendingTarget().IsUnknown());
}

TEST_P(CliClient, AppUpdateRollback) {
  auto akclient{createAkClient()};
  auto initial_target{akclient->GetCurrent()};
  auto app01 = registry.addApp(
      fixtures::ComposeApp::create("app-01", "service-01", "image-01", fixtures::ComposeApp::ServiceTemplate,
                                   Docker::ComposeAppEngine::ComposeFile, "compose-start-failure"));
  auto target01 = Target::toTufTarget(createAppTarget({app01}));

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::ExitCode::InstallRollbackOk);
  ASSERT_EQ(akclient->GetCurrent(), initial_target);
  ASSERT_TRUE(akclient->CheckAppsInSync() == nullptr);
}

TEST_P(CliClient, OstreeUpdateRollback) {
  auto akclient{createAkClient()};

  // do initial to update to run some Apps
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto target01 = Target::toTufTarget(createAppTarget({app01}));

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::ExitCode::Ok);
  ASSERT_EQ(akclient->GetCurrent(), target01);

  auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  std::vector<AppEngine::App> apps_updated{app01_updated};
  auto target02 = Target::toTufTarget(createTarget(&apps_updated));

  ASSERT_EQ(cli::Install(*akclient, target02.Version()), cli::ExitCode::InstallNeedsReboot);
  // deploy the previous version/commit to emulate rollback
  getSysRepo().deploy(target01.Sha256Hash());
  reboot(akclient);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::ExitCode::InstallRollbackOk);
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
