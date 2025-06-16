#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>

#include "test_utils.h"
#include "utilities/utils.h"

#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"
#include "target.h"
#include "uptane_generator/image_repo.h"

#include "fixtures/aklitetest.cc"

TEST_P(AkliteTest, RollbackIfAppsInstallFailsAndPowerCut) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  std::vector<AppEngine::App> apps{app01};
  auto target_01 = createTarget(&apps);

  {
    // update to the latest version
    update(*client, getInitialTarget(), target_01);
  }

  {
    // reboot and make sure that the update succeeded
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
  }

  {
    // create a new "bad" Target, it includes both ostree and app update, App is invalid
    auto app01_updated = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "compose-failure"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createTarget(&apps);

    // try to update to the latest version, it must fail because App is invalid
    update(*client, target_01, target_02, data::ResultCode::Numeric::kInstallFailed);
    // since new sysroot (target_02) was installed (deployed) successfully then we expect that
    // there is a corresponding pending deployment
    ASSERT_EQ(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending), target_02.sha256Hash());

    // emulate power cut
    reboot(client);
    // getCurrent() "thinks" that target_02 is current because a device is booted on it.
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_02));
    // but it's also a failing Target since its installation had failed before the power cut occurred
    ASSERT_TRUE(client->isRollback(client->getCurrent()));
    ASSERT_TRUE(client->isRollback(target_02));
    // emulate rollback triggered by daemon_main
    ASSERT_TRUE(targetsMatch(client->getRollbackTarget(), target_01));
    update(*client, target_02, target_01, data::ResultCode::Numeric::kNeedCompletion);
    ASSERT_EQ(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending), target_01.sha256Hash());
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
    ASSERT_TRUE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty());
  }

  {
    // finally do a valid App update
    auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-04"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_03 = createAppTarget(apps, target_01);
    updateApps(*client, target_01, target_03);

    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_03));
    ASSERT_TRUE(app_engine->isRunning(app01_updated));
    ASSERT_TRUE(client->appsInSync(client->getCurrent()));
    ASSERT_TRUE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty());
  }
}

TEST_P(AkliteTest, RollbackIfAppsInstallFailsNoContainer) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  std::vector<AppEngine::App> apps{app01};
  auto target_01 = createTarget(&apps);

  {
    // update to the latest version
    update(*client, getInitialTarget(), target_01);
  }

  {
    // reboot and make sure that the update succeeded
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
  }

  {
    // create a new "bad" Target, it includes both ostree and app update, App is invalid
    auto app01_updated = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "container-failure"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createTarget(&apps);

    // try to update to the latest version, it must fail because App is invalid
    update(*client, target_01, target_02, data::ResultCode::Numeric::kInstallFailed, {DownloadResult::Status::Ok, ""},
           "App containers haven't been created");

    // emulate next iteration/update cycle of daemon_main
    client->checkForUpdatesBegin();
    ASSERT_TRUE(client->isRollback(target_02));
    ASSERT_FALSE(client->appsInSync(client->getCurrent()));
    // sync target_01 apps
    updateApps(*client, client->getCurrent(), client->getCurrent());
    client->checkForUpdatesEnd(target_01);

    // make sure the initial target_01 is running
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }

  {
    // create a new "bad" Target, it includes just app update, App is invalid
    auto app01_updated = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-03", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "container-failure"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createAppTarget(apps);

    // try to update to the latest version, it must fail because App is invalid
    updateApps(*client, target_01, target_02, DownloadResult::Status::Ok, "",
               data::ResultCode::Numeric::kInstallFailed);

    // emulate next iteration/update cycle of daemon_main
    client->checkForUpdatesBegin();
    ASSERT_TRUE(client->isRollback(target_02));
    ASSERT_FALSE(client->appsInSync(client->getCurrent()));
    // sync target_01 apps
    updateApps(*client, client->getCurrent(), client->getCurrent());
    client->checkForUpdatesEnd(target_01);

    // make sure the initial target_01 is running
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }
}

/**
 * @brief Test rollback if new version App failed to start just after successful
 *        boot on a new sysroot version and power cut occurs
 *
 * 1. Initiate an update to a new Target that includes both sysroot/ostree and App update
 * 2. Download and install steps are successful
 * 3. Reboot on the new sysroot version is successful
 * 4. Failure to start the updated App occurs on aklite start
 * 5. Mark the new Target as a failing Target (finalization is completed)
 * 6. Power cut
 * 6. Boot again
 * 7. Since finalization has been completed before the power cut then no finalization anymore
 * 8. The current target is marked as a failing Target hence a rollback to the previous version is initiated
 * 9. Reboot again and do a normal/successful finalization of the initial valid Target
 */

TEST_P(AkliteTest, OstreeAndAppRollbackIfAppsStartFailsAndPowerCut) {
  // boot device
  const auto app_engine_type{GetParam()};
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  std::vector<AppEngine::App> apps{app01};
  auto target_01 = createTarget(&apps);

  {
    // update to the latest version
    update(*client, getInitialTarget(), target_01);
  }

  {
    // reboot and make sure that the update succeeded
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
    ASSERT_FALSE(client->isRollback(target_01));
  }

  // create a new "bad" Target, it includes both an ostree and app update, App is invalid,
  // specifically its creation is successful but it fails to start after reboot caused by the ostree update
  auto app01_updated = registry.addApp(
      fixtures::ComposeApp::create("app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate,
                                   Docker::ComposeAppEngine::ComposeFile, "compose-start-failure"));
  std::vector<AppEngine::App> apps_updated{app01_updated};
  auto target_02 = createTarget(&apps_updated);

  {
    // update to the latest version, it succeeds, assumption is that Apps' containers creation does not fail
    update(*client, target_01, target_02, data::ResultCode::Numeric::kNeedCompletion);

    // make sure that target_01 is still current because a reboot is required to apply target_01
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    // app01 should be stopped at this point since its containers re-creation had happened
    ASSERT_FALSE(app_engine->isRunning(app01));

    // Both App version should be fetched until the new version is successfully started or rollback
    ASSERT_TRUE(app_engine->isFetched(app01_updated));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01));
    }
  }

  {
    boost::filesystem::remove(fixtures::ClientTest::test_dir_.Path() / "need_reboot");
    client = createLiteClient(InitialVersion::kOff, boost::none, false);
    ASSERT_FALSE(client->finalizeInstall());

    // for some reason ostreemanager::getCurrent() is driven by currently booted ostree hash,
    // so it thinks that current version is target_02
    // target_02 is current since a device is booted on it, at the same time it is "rollback"/failing
    // target since it's partially installed, just ostree
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_02));
    ASSERT_TRUE(targetsMatch(client->getRollbackTarget(), target_01));
    ASSERT_TRUE(client->isRollback(target_02));

    // Still two versions of Apps should be present
    ASSERT_TRUE(app_engine->isFetched(app01_updated));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01));
    }
  }
  {
    // emulate power cut just after finalization followed up by boot
    boost::filesystem::remove(fixtures::ClientTest::test_dir_.Path() / "need_reboot");
    client = createLiteClient(InitialVersion::kOff, boost::none, false);

    // since the finalization for target_02 has completed before then there is no any pending installation,
    // thus the finalizeInstall() doesn't do anything
    ASSERT_TRUE(client->finalizeInstall());

    // Still two versions of Apps should be present
    ASSERT_TRUE(app_engine->isFetched(app01_updated));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01));
    }

    // New Target is considered as current since a device is booted on it
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_02));
    ASSERT_TRUE(client->isRollback(target_02));
    // if current target is "rollback" target then the rollback is triggered
    ASSERT_TRUE(targetsMatch(client->getRollbackTarget(), target_01));
    update(*client, target_02, target_01, data::ResultCode::Numeric::kNeedCompletion);

    // Still two versions of Apps should be present since Apps are not started yet
    ASSERT_TRUE(app_engine->isFetched(app01));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01_updated));
    }
  }
  {
    // emulate power cut just after finalization, reboot and make sure that the rollback succeeds
    reboot(client);  // it includes finalization
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);

    // now only one version of App is stored/fetched for both types of Apps
    ASSERT_TRUE(app_engine->isFetched(app01));
    ASSERT_FALSE(app_engine->isFetched(app01_updated));

    ASSERT_TRUE(app_engine->isRunning(app01));

    ASSERT_FALSE(client->isRollback(target_01));
    ASSERT_TRUE(client->isRollback(target_02));
  }
}

INSTANTIATE_TEST_SUITE_P(MultiEngine, AkliteTest, ::testing::Values("ComposeAppEngine", "RestorableAppEngine"));

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
