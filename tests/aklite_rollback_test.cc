#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "test_utils.h"
#include "utilities/utils.h"

#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"
#include "target.h"
#include "uptane_generator/image_repo.h"

#include "fixtures/aklitetest.cc"

TEST_P(AkliteTest, OstreeAndAppUpdateIfRollback) {
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
    ASSERT_TRUE(daemon_.areContainersCreated());
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
  }

  {
    // update app, change image URL
    auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createTarget(&apps);
    // update to the latest version
    update(*client, target_01, target_02);
    ASSERT_TRUE(daemon_.areContainersCreated());
    // deploy the previous version/commit to emulate rollback
    getSysRepo().deploy(target_01.sha256Hash());

    reboot(client);
    // make sure that a rollback has happened and a client is still running the previous Target
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    // we stopped the original app before update
    ASSERT_FALSE(app_engine->isRunning(app01));
    ASSERT_FALSE(app_engine->isRunning(app01_updated));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);

    // emulate do_app_sync
    updateApps(*client, target_01, client->getCurrent());
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }
}

TEST_P(AkliteTest, OstreeAndAppUpdateIfRollbackAndAfterBootRecreation) {
  setCreateContainersBeforeReboot(false);
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
    ASSERT_FALSE(daemon_.areContainersCreated());
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
  }

  {
    // update app, change image URL
    auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createTarget(&apps);
    // update to the latest version
    update(*client, target_01, target_02);
    // deploy the previous version/commit to emulate rollback
    getSysRepo().deploy(target_01.sha256Hash());

    reboot(client);
    // make sure that a rollback has happened and a client is still running the previous Target
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));

    ASSERT_FALSE(app_engine->isRunning(app01_updated));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);

    // emulate do_app_sync
    updateApps(*client, target_01, client->getCurrent());
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }
}

TEST_P(AkliteTest, RollbackIfOstreeInstallFails) {
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
    // create a new "bad" Target, it includes both ostree and app update, rootfs is invalid
    auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
    std::vector<AppEngine::App> apps{app01_updated};

    TemporaryDirectory broken_rootfs_dir;
    auto target_02 = createTarget(&apps, "", broken_rootfs_dir.PathString());

    // try to update to the latest version, it must fail because the target's rootfs is invalid (no kernel)
    update(*client, target_01, target_02, data::ResultCode::Numeric::kInstallFailed, {DownloadResult::Status::Ok, ""},
           "Failed to find kernel", false);

    // emulate next iteration/update cycle of daemon_main
    client->checkForUpdatesBegin();
    ASSERT_TRUE(client->isRollback(target_02));
    const auto app_engine_type{GetParam()};
    if (app_engine_type == "RestorableAppEngine") {
      // a download process doesn't "break" currently installed and running retsorable apps
      // appsInSync cleans any unneeded layers stored in the skopeo/OCI store
      ASSERT_TRUE(client->appsInSync(client->getCurrent()));
    } else {
      ASSERT_FALSE(client->appsInSync(client->getCurrent()));
      // sync target_01 apps
      updateApps(*client, client->getCurrent(), client->getCurrent());
    }
    client->checkForUpdatesEnd(target_01);

    // make sure the initial target_01 is running
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }
}

TEST_P(AkliteTest, RollbackIfAppsInstallFails) {
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
    // since new sysroot (target_02) was installed (deployed) succeesfully then we expect that
    // there is a corresponding pending deployment
    ASSERT_EQ(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending), target_02.sha256Hash());

    // emulate daemon_main's logic in the case of kInstallFailed
    client->setAppsNotChecked();
    // emulate next iteration/update cycle of daemon_main
    client->checkForUpdatesBegin();
    ASSERT_TRUE(client->isRollback(target_02));
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_FALSE(client->appsInSync(client->getCurrent()));
    // sync target_01 apps
    updateApps(*client, client->getCurrent(), client->getCurrent());
    client->checkForUpdatesEnd(target_01);

    // make sure the initial target_01 is running
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    // ASSERT_TRUE(app_engine->isRunning(app01));
    // make sure that target_02 is not pending anymore
    ASSERT_NE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending), target_02.sha256Hash());
    // and there is no any pending deployment at all
    ASSERT_TRUE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty());
  }

  {
    // create a new "bad" Target, it includes just app update, App is invalid
    auto app01_updated = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-03", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "compose-failure"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createAppTarget(apps, target_01);

    // try to update to the latest version, it must fail because App is invalid
    ASSERT_TRUE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty());
    updateApps(*client, target_01, target_02, DownloadResult::Status::Ok, "",
               data::ResultCode::Numeric::kInstallFailed);
    ASSERT_TRUE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty());

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

TEST_P(AkliteTest, AppRollbackIfAppsInstallFails) {
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
    ASSERT_FALSE(client->isRollback(target_01));
  }

  {
    // create a new "bad" Target, it includes just app update, App is invalid
    auto app01_updated = registry.addApp(
        fixtures::ComposeApp::create("app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate,
                                     Docker::ComposeAppEngine::ComposeFile, "compose-failure"));
    std::vector<AppEngine::App> apps{app01_updated};
    auto target_02 = createAppTarget(apps);

    // try to update to the latest version, it must fail because App is invalid
    updateApps(*client, target_01, target_02, DownloadResult::Status::Ok, "", data::ResultCode::Numeric::kInstallFailed,
               "failed to bring Compose App up");

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
    // make sure that the bad target is still "known locally" (marked as a failing target)
    ASSERT_TRUE(client->isRollback(target_02));
  }
}

/**
 * @brief Test rollback if new version App failed to start just after succcessful boot on a new sysroot version
 *
 * 1. Initiate an update to a new Target that includes both sysroot/ostree and App update
 * 2. Download and install steps are successful
 * 3. Reboot on the new sysroot version is successful
 * 4. Failure to start the updated App occurs on aklite start
 * 5. Mark the new Target as a failing Target
 * 6. Trigger rollback to the previous successful Target
 * 7. Check whether the previous Target has been successfully installed after reboot
 */

TEST_P(AkliteTest, OstreeAndAppRollbackIfAppsStartFails) {
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
  // specifically its creation is succesful but it fails to start after reboot caused by the ostree update
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
    // app01 should be stopped at this point since its containers re-creation had happenned
    ASSERT_FALSE(app_engine->isRunning(app01));

    // Both App version should be fetched/present until the new version is successfully started or rollback
    ASSERT_TRUE(app_engine->isFetched(app01_updated));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01));
    }
  }

  {
    boost::filesystem::remove(fixtures::ClientTest::test_dir_.Path() / "need_reboot");
    device_gateway_.resetEvents(client->http_client);
    client = createLiteClient(InitialVersion::kOff, boost::none, false);

    ASSERT_FALSE(client->finalizeInstall());
    // make sure that report events have been sent and EcuInstallationCompleted contains the error message
    checkEvents(*client, target_01, UpdateType::kFinalized, "", "failed to bring Compose App up");

    // for some reason ostreemanager::getCurrent() is driven by currently booted ostree hash,
    // so it thinks that current version is target_02
    // target_02 is current since a device is booted on it, at the same time it is "rollback"/failing
    // target since it's partially installed, just ostree
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_02));
    ASSERT_TRUE(targetsMatch(client->getRollbackTarget(), target_01));
    ASSERT_TRUE(client->isRollback(target_02));

    // Both App version should be fetched/present until the new version is successfully started or rollback
    ASSERT_TRUE(app_engine->isFetched(app01_updated));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01));
    }

    update(*client, target_02, target_01, data::ResultCode::Numeric::kNeedCompletion);

    // Both App version should be fetched/present until the new version is successfully started or rollback
    ASSERT_TRUE(app_engine->isFetched(app01));
    // Unlike Restorable Apps, Compose App cannot have two versions that co-exist at the same
    if (app_engine_type == "RestorableAppEngine") {
      ASSERT_TRUE(app_engine->isFetched(app01_updated));
    }
  }

  {
    // reboot and make sure that the update succeeded
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
    checkEvents(*client, target_01, UpdateType::kOstree);
    ASSERT_TRUE(app_engine->isRunning(app01));
    ASSERT_FALSE(client->isRollback(target_01));
    ASSERT_TRUE(client->isRollback(target_02));

    // just one version should be present on a device after successful installation
    ASSERT_TRUE(app_engine->isFetched(app01));
    ASSERT_FALSE(app_engine->isFetched(app01_updated));
  }
}

/**
 * @brief Test rollback if new version App failed to start just after succcessful
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
 * 9. Reboot again and do a normal/succesful finalization of the initial valid Target
 */

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
