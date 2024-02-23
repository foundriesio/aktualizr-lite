#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/format.hpp>
#include <boost/process.hpp>

#include "aktualizr-lite/cli/cli.h"
#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "fixtures/aklitetest.cc"

using namespace aklite;

class CliClient : public AkliteTest {
 protected:
  std::shared_ptr<AkliteClient> createAkClient(InitialVersion initial_version = InitialVersion::kOn) {
    return std::make_shared<AkliteClient>(createLiteClient(initial_version));
  }

  TufTarget createTufTarget(AppEngine::App* app = nullptr, const std::string& hwid = "", bool just_app_target = false) {
    std::vector<AppEngine::App> apps;
    if (!app) {
      apps.emplace_back(registry.addApp(fixtures::ComposeApp::create("app-01")));
    } else {
      apps.push_back(*app);
    }
    return just_app_target ? Target::toTufTarget(createAppTarget(apps))
                           : Target::toTufTarget(createTarget(&apps, hwid));
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
    conf.pacman.extra[RootfsTreeManager::Config::UpdateBlockParamName] = "1";
    if (!tag_.empty()) {
      conf.pacman.extra["tags"] = tag_;
    }
    if (!hardware_id_.empty()) {
      conf.provision.primary_ecu_hardware_id = hardware_id_;
    }
  }

 protected:
  std::string tuf_repo_server_{device_gateway_.getTufRepoUri()};
  std::string ostree_server_uri_{device_gateway_.getOsTreeUri()};
  std::string tag_;
  std::string hardware_id_;
};

TEST_P(CliClient, FullUpdate) {
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallNeedsReboot);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::InstallNeedsReboot);
  reboot(akclient);
  ASSERT_TRUE(akclient->IsInstallationInProgress());
  ASSERT_EQ(akclient->GetPendingTarget(), target01);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::Ok);
}

TEST_P(CliClient, AppOnlyUpdate_01) {
  auto akclient{createAkClient()};
  const auto target01 = createTufTarget(nullptr, "", true);

  ASSERT_EQ(cli::Install(*akclient, target01.Version(), "", InstallMode::OstreeOnly),
            cli::StatusCode::InstallAppsNeedFinalization);
  ASSERT_TRUE(akclient->IsInstallationInProgress());
  ASSERT_EQ(akclient->GetPendingTarget(), target01);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::Ok);
}

TEST_P(CliClient, AppOnlyUpdate_02) {
  auto akclient{createAkClient()};
  const auto target01 = createTufTarget(nullptr, "", true);
  ASSERT_EQ(cli::Install(*akclient, target01.Version(), "", InstallMode::All), cli::StatusCode::Ok);
}

TEST_P(CliClient, NoMatchingTufTargets_Tag) {
  tag_ = "device-tag";
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::CheckinNoMatchingTargets);
}

TEST_P(CliClient, NoMatchingTufTargets_HardwareId) {
  hardware_id_ = "some-other-hwid";
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::CheckinNoMatchingTargets);
}

TEST_P(CliClient, TufMetaDownloadFailure) {
  // make the TUF server URI invalid so the TUF metadata update fails
  tuf_repo_server_ = device_gateway_.getTufRepoUri() + "/foobar";
  auto akclient{createAkClient()};
  const auto target01{createTufTarget()};

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::CheckinMetadataFetchFailure);
}

TEST_P(CliClient, TufTargetNotFoundInvalidHardwareId) {
  auto akclient{createAkClient()};
  auto target01 = createTufTarget(nullptr, "foobar-hwid");
  // The TUF update is successful and there is one/initial Target that matches a device's hardware ID, so
  // the checkin is successful.
  // However, the specified target to install `target01` is not among the valid TUF targets,
  // so the install gets TufTargetNotFound.
  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::TufTargetNotFound);
}

TEST_P(CliClient, TufTargetNotFoundInvalidVersion) {
  auto akclient{createAkClient()};
  auto target01 = createTufTarget(nullptr);

  // The TUF update is successful and there are Targets that matches a device's hardware ID, so
  // the checkin is successful.
  // However, the specified target to install, target v100, is not among the valid TUF targets,
  // so the install gets TufTargetNotFound.
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
  // make sure that the installation hasn't happened
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

TEST_P(CliClient, AppUpdateRollback) {
  auto akclient{createAkClient()};
  auto initial_target{akclient->GetCurrent()};
  auto app01 = registry.addApp(
      fixtures::ComposeApp::create("app-01", "service-01", "image-01", fixtures::ComposeApp::ServiceTemplate,
                                   Docker::ComposeAppEngine::ComposeFile, "compose-start-failure"));
  const auto target01 = createTufTarget(&app01, "", true);

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallRollbackOk);
  ASSERT_EQ(akclient->GetCurrent(), initial_target);
  ASSERT_TRUE(akclient->CheckAppsInSync() == nullptr);
}

TEST_P(CliClient, OstreeUpdateRollback) {
  auto akclient{createAkClient()};

  // do initial to update to run some Apps
  const auto target01 = createTufTarget(nullptr, "", true);
  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::Ok);
  ASSERT_EQ(akclient->GetCurrent(), target01);

  auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  auto target02 = createTufTarget(&app01_updated);

  ASSERT_EQ(cli::Install(*akclient, target02.Version()), cli::StatusCode::InstallNeedsReboot);
  // deploy the previous version/commit to emulate the bootloader driven rollback
  getSysRepo().deploy(target01.Sha256Hash());
  reboot(akclient);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::InstallRollbackOk);
  ASSERT_TRUE(akclient->IsRollback(target02));
  ASSERT_EQ(akclient->GetCurrent(), target01);
  ASSERT_EQ(akclient->CheckAppsInSync(), nullptr);
}

TEST_P(CliClient, FullUpdateAppDrivenRollback) {
  // setCreateContainersBeforeReboot(false);
  auto akclient{createAkClient()};

  // do initial to update to run some Apps
  const auto target01 = createTufTarget(nullptr, "", true);
  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::Ok);
  ASSERT_EQ(akclient->GetCurrent(), target01);

  ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::Ok);
  ASSERT_EQ(akclient->GetCurrent(), target01);

  auto app01 = registry.addApp(
      fixtures::ComposeApp::create("app-01", "service-01", "image-01", fixtures::ComposeApp::ServiceTemplate,
                                   Docker::ComposeAppEngine::ComposeFile, "compose-start-failure"));
  const auto target02 = createTufTarget(&app01);

  ASSERT_EQ(cli::Install(*akclient, target02.Version()), cli::StatusCode::InstallNeedsReboot);
  reboot(akclient);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::InstallRollbackNeedsReboot);
  reboot(akclient);
  ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::Ok);
  ASSERT_TRUE(akclient->IsRollback(target02));
  ASSERT_EQ(akclient->GetCurrent(), target01);
  ASSERT_EQ(akclient->CheckAppsInSync(), nullptr);
}

TEST_P(CliClient, OstreeRollbackToInitialTarget) {
  for (const auto& init_ver_stat : std::vector<InitialVersion>{InitialVersion::kOff, InitialVersion::kOn}) {
    auto akclient{createAkClient(init_ver_stat)};
    auto initial_target{akclient->GetCurrent()};
    auto target01 = createTufTarget();

    ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallNeedsReboot);
    // deploy the previous version/commit to emulate rollback
    getSysRepo().deploy(initial_target.Sha256Hash());
    reboot(akclient);
    ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::InstallRollbackOk);
    ASSERT_TRUE(akclient->IsRollback(target01));
    ASSERT_EQ(akclient->GetCurrent(), initial_target);
    ASSERT_EQ(akclient->CheckAppsInSync(), nullptr);
  }
}

TEST_P(CliClient, AppRollbackToInitialTarget) {
  for (const auto& init_ver_stat : std::vector<InitialVersion>{InitialVersion::kOff, InitialVersion::kOn}) {
    auto akclient{createAkClient(init_ver_stat)};
    auto initial_target{akclient->GetCurrent()};
    auto app01 = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-01", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "compose-start-failure"));
    auto target01 = createTufTarget(&app01, "", true);

    ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallRollbackOk);
    ASSERT_EQ(akclient->GetCurrent(), initial_target);
    ASSERT_TRUE(akclient->IsRollback(target01));
    ASSERT_EQ(akclient->CheckAppsInSync(), nullptr);
    ASSERT_FALSE(app_engine->isRunning(app01));
  }
}

TEST_P(CliClient, OstreeAndAppRollbackToInitialTarget) {
  for (const auto& init_ver_stat : std::vector<InitialVersion>{InitialVersion::kOff, InitialVersion::kOn}) {
    auto akclient{createAkClient(init_ver_stat)};
    auto initial_target{akclient->GetCurrent()};

    auto app01 = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-01", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "compose-start-failure"));
    auto target01 = createTufTarget(&app01);

    ASSERT_EQ(cli::Install(*akclient, target01.Version()), cli::StatusCode::InstallNeedsReboot);
    reboot(akclient);
    ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::InstallRollbackNeedsReboot);
    reboot(akclient);
    ASSERT_EQ(cli::CompleteInstall(*akclient), cli::StatusCode::Ok);
    ASSERT_EQ(akclient->GetCurrent(), initial_target);
    ASSERT_TRUE(akclient->IsRollback(target01));
    ASSERT_FALSE(app_engine->isRunning(app01));
    ASSERT_EQ(akclient->CheckAppsInSync(), nullptr);
  }
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
