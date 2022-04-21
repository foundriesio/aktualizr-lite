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
      setAvailableStorageSpace(std::numeric_limits<boost::uintmax_t>::max());
      app_engine = std::make_shared<Docker::RestorableAppEngine>(
          fixtures::ClientTest::test_dir_.Path() / "apps-store", apps_root_dir, daemon_.dataRoot(), registry_client_,
          docker_client_, registry.getSkopeoClient(), daemon_.getUrl(), compose_cmd,
          [this](const boost::filesystem::path& path) {
            return std::tuple<boost::uintmax_t, boost::uintmax_t>{this->available_storage_space_,
                                                                  (this->watermark_ * this->available_storage_space_)};
          });
    } else {
      throw std::invalid_argument("Unsupported AppEngine type: " + app_engine_type);
    }
  }

  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
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

  void setAvailableStorageSpace(const boost::uintmax_t& space_size) { available_storage_space_ = space_size; }
  void setCreateContainersBeforeReboot(bool value) { create_containers_before_reboot_ = value; }

 private:
  boost::uintmax_t available_storage_space_;
  float watermark_{0.8};
  bool create_containers_before_reboot_{true};
};

TEST_P(AkliteTest, OstreeUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto new_target = createTarget();

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  checkEvents(*client, new_target, UpdateType::kOstree);
  ASSERT_FALSE(app_engine->isRunning(app01));
}

TEST_P(AkliteTest, AppUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));

  // update app and add new one
  auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02", "service-01", "factory/image-01"));
  auto app03 = registry.addApp(fixtures::ComposeApp::create("app-03", "service-01", "foo/bar/wierd/image-01"));
  auto target02 = createAppTarget({app01_updated, app02, app03});
  updateApps(*client, target01, target02);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target02));
  ASSERT_TRUE(app_engine->isRunning(app01_updated));
}

TEST_P(AkliteTest, AppUpdateWithoutLayerManifest) {
  // If a manifest with a layer list is not present an update should succeed anyway, so
  // the "size-aware" aklite can download Targets created before the "size-aware" compose-publish is deployed.
  auto app01 = registry.addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", Json::Value()));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));

  // update app
  auto app01_updated = registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02"));
  auto target02 = createAppTarget({app01_updated});
  updateApps(*client, target01, target02);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target02));
  ASSERT_TRUE(app_engine->isRunning(app01_updated));
}

TEST_P(AkliteTest, AppRemoval) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02"));

  auto client =
      createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{"app-01", "app-02"}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));

  auto target01 = createAppTarget({app01, app02});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));
  ASSERT_TRUE(app_engine->isRunning(app02));

  reboot(client, boost::make_optional(std::vector<std::string>{"app-01"}));
  // make sure the "hadleRemovedApps" is called
  client->appsInSync();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  checkHeaders(*client, target01);
  checkEvents(*client, target01, UpdateType::kApp);
  ASSERT_TRUE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isFetched(app02));
  ASSERT_FALSE(app_engine->isRunning(app02));
}

TEST_P(AkliteTest, AppInvalidUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));

  // update app
  auto app01_updated = registry.addApp(fixtures::ComposeApp::create(
      "app-01", "service-01", "image-02", fixtures::ComposeApp::ServiceTemplate, "incorrect-compose-file.yml"));
  auto target02 = createAppTarget({app01_updated});
  updateApps(*client, target01, target02, DownloadResult::Status::DownloadFailed);
  ASSERT_FALSE(targetsMatch(client->getCurrent(), target02));

  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));
}

TEST_P(AkliteTest, AppDownloadFailure) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});
  auto invalid_custom_target{target01.custom_data()};
  invalid_custom_target["docker_compose_apps"]["app-01"]["uri"] =
      "hub.foundries.io/factory/app-01@sha256:badhash5501792d4eeb043b728c9a0c8417fbe9f62146625610377e11bcf572d";
  target01.updateCustom(invalid_custom_target);

  updateApps(*client, getInitialTarget(), target01, DownloadResult::Status::DownloadFailed, "Not Found");
}

TEST_P(AkliteTest, OstreeDownloadFailure) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto new_target = createTarget();
  const Uptane::Target invalid_target{
      new_target.filename(), new_target.ecus(), {Hash{Hash::Type::kSha256, "foobarhash"}}, 0, "", "OSTREE"};

  // update to the latest version
  update(*client, getInitialTarget(), invalid_target, data::ResultCode::Numeric::kDownloadFailed,
         {DownloadResult::Status::DownloadFailed, "404"});
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
}

TEST_P(AkliteTest, OstreeAndAppUpdate) {
  // App's containers are re-created before reboot
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  std::vector<AppEngine::App> apps{app01};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_TRUE(daemon_.areContainersCreated());

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  ASSERT_TRUE(app_engine->isRunning(app01));
  checkEvents(*client, new_target, UpdateType::kOstree);
}

TEST_P(AkliteTest, OstreeAndAppUpdateIfCreateAfterBoot) {
  // App's containers are re-created after reboot
  setCreateContainersBeforeReboot(false);
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  std::vector<AppEngine::App> apps{app01};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(daemon_.areContainersCreated());

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  ASSERT_TRUE(app_engine->isRunning(app01));
  checkEvents(*client, new_target, UpdateType::kOstree);
}

TEST_P(AkliteTest, OstreeAndAppUpdateWithShortlist) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02"));

  auto client = createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{"app-02"}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // create new Target, both ostree and two Apps update
  std::vector<AppEngine::App> apps{app01, app02};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  checkEvents(*client, new_target, UpdateType::kOstree);
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_TRUE(app_engine->isRunning(app02));
}

TEST_P(AkliteTest, OstreeAndAppUpdateWithEmptyShortlist) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));
  auto app02 = registry.addApp(fixtures::ComposeApp::create("app-02"));

  auto client = createLiteClient(InitialVersion::kOn, boost::make_optional(std::vector<std::string>{""}));
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // create new Target, both ostree and two Apps update
  std::vector<AppEngine::App> apps{app01, app02};
  auto new_target = createTarget(&apps);

  // update to the latest version
  update(*client, getInitialTarget(), new_target);
  // make sure that the installed Target is not "finalized"/applied and Apps are not running
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  checkEvents(*client, new_target, UpdateType::kOstree);
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));
}

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

TEST_P(AkliteTest, InvalidAppComposeUpdate) {
  // invalid service definition, `ports` value must be integer
  const std::string AppInvalidServiceTemplate = R"(
      %s:
        image: %s
        ports:
          - foo:bar)";

  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));
  ASSERT_FALSE(app_engine->isRunning(app01));

  auto target01 = createAppTarget({app01});

  updateApps(*client, getInitialTarget(), target01);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  ASSERT_TRUE(app_engine->isRunning(app01));

  // update app
  auto app01_updated =
      registry.addApp(fixtures::ComposeApp::create("app-01", "service-01", "image-02", AppInvalidServiceTemplate));
  auto target02 = createAppTarget({app01_updated});

  const auto app_engine_type{GetParam()};

  // in the case of Restorable App we expect that download/fetch is successfull
  DownloadResult::Status expected_download_res{DownloadResult::Status::Ok};
  if (app_engine_type == "ComposeAppEngine") {
    // App is verified (docker-compose config) at the "fetch" phase for ComposeAppEngine
    // this is a bug that became a feature bug, so let's adjust to it
    expected_download_res = DownloadResult::Status::DownloadFailed;
  }
  // updateApps() emulates LiteClient's client which invokes the fecthed Target verification.
  // the verification is supposed to fail and the installation process is never invoked
  updateApps(*client, target01, target02, expected_download_res, "", data::ResultCode::Numeric::kVerificationFailed);
  ASSERT_FALSE(targetsMatch(client->getCurrent(), target02));

  ASSERT_TRUE(targetsMatch(client->getCurrent(), target01));
  if (app_engine_type == "RestorableAppEngine") {
    // make sure that the update with invalid App compose file didn't break currently running App
    // it works only for RestorableAppEngine because in the case of ComposeAppEngine
    // App content in <compose-apps>/<app-dir> has been already replaced with invalid app01_updated, so
    // there is no means to check if app01 is running (there is no its docker-compose.yml of file system)
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
    update(*client, target_01, target_02, data::ResultCode::Numeric::kInstallFailed, {DownloadResult::Status::Ok, ""});

    // emulate next iteration/update cycle of daemon_main
    client->checkForUpdatesBegin();
    ASSERT_TRUE(client->isRollback(target_02));
    const auto app_engine_type{GetParam()};
    if (app_engine_type == "RestorableAppEngine") {
      // a download process doesn't "break" currently installed and running retsorable apps
      // appsInSync cleans any unneeded layers stored in the skopeo/OCI store
      ASSERT_TRUE(client->appsInSync());
    } else {
      ASSERT_FALSE(client->appsInSync());
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
    ASSERT_FALSE(client->appsInSync());
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
    ASSERT_FALSE(client->appsInSync());
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
    ASSERT_TRUE(client->appsInSync());
    ASSERT_TRUE(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty());
  }
}

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
    // since new sysroot (target_02) was installed (deployed) succeesfully then we expect that
    // there is a corresponding pending deployment
    ASSERT_EQ(client->sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending), target_02.sha256Hash());

    // emulate power cut
    reboot(client);
    // getCurrent() "thinks" that target_02 is current beacuse a device is booted on it.
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_02));
    // but it's also a failing Target since its installation had failed before the power cut occured
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
    ASSERT_TRUE(client->appsInSync());
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
    ASSERT_FALSE(client->appsInSync());
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
    ASSERT_FALSE(client->appsInSync());
    // sync target_01 apps
    updateApps(*client, client->getCurrent(), client->getCurrent());
    client->checkForUpdatesEnd(target_01);

    // make sure the initial target_01 is running
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
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
    ASSERT_FALSE(client->appsInSync());
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
    device_gateway_.resetEvents();
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

    // now onlye one version of App is stored/fetched for both types of Apps
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
