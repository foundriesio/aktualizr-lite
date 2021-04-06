#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "test_utils.h"
#include "utilities/utils.h"

#include "composeappmanager.h"
#include "liteclient.h"
#include "target.h"
#include "uptane_generator/image_repo.h"

#include "fixtures/composeappenginetest.cc"
#include "fixtures/liteclienttest.cc"

class AkliteTest : public fixtures::ClientTest, public fixtures::AppEngineTest {
 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none) override {
    return ClientTest::createLiteClient(app_engine, initial_version, apps, apps_root_dir.string());
  }

 private:
};

TEST_F(AkliteTest, OstreeUpdate) {
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
  ASSERT_FALSE(app_engine->isRunning(app01));
}

TEST_F(AkliteTest, AppUpdate) {
  auto app01 = registry.addApp(fixtures::ComposeApp::create("app-01"));

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

TEST_F(AkliteTest, OstreeAndAppUpdate) {
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

  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
  ASSERT_TRUE(app_engine->isRunning(app01));
}

TEST_F(AkliteTest, OstreeAndAppUpdateWithShortlist) {
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
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_TRUE(app_engine->isRunning(app02));
}

TEST_F(AkliteTest, OstreeAndAppUpdateWithEmptyShortlist) {
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
  ASSERT_FALSE(app_engine->isRunning(app01));
  ASSERT_FALSE(app_engine->isRunning(app02));
}

TEST_F(AkliteTest, OstreeAndAppUpdateIfRollback) {
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
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    checkHeaders(*client, target_01);
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
    // we stopped the original app before update
    ASSERT_FALSE(app_engine->isRunning(app01));
    ASSERT_FALSE(app_engine->isRunning(app01_updated));
    checkHeaders(*client, target_01);

    // emulate do_app_sync
    updateApps(*client, target_01, client->getCurrent());
    ASSERT_TRUE(targetsMatch(client->getCurrent(), target_01));
    ASSERT_TRUE(app_engine->isRunning(app01));
  }
}

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
