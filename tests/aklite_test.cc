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
  client->appsInSync(client->getCurrent());
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
