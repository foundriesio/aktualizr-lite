#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "crypto/p11engine.h"
#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "uptane_generator/image_repo.h"
#include "utilities/utils.h"

#include "composeappmanager.h"
#include "liteclient.h"

#include <iostream>
#include <string>

#include "helpers.h"
#include "ostree/repo.h"
#include "target.h"

#include "fixtures/liteclienthsmtest.cc"

using ::testing::NiceMock;

class LiteClientHSMTest : public fixtures::ClientHSMTest {
 protected:
  std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               bool finalize = true) override {
    app_engine_mock_ = std::make_shared<NiceMock<fixtures::MockAppEngine>>();
    return ClientHSMTest::createLiteClient(app_engine_mock_, initial_version, apps);
  }

  std::shared_ptr<NiceMock<fixtures::MockAppEngine>>& getAppEngine() { return app_engine_mock_; }

 private:
  std::shared_ptr<NiceMock<fixtures::MockAppEngine>> app_engine_mock_;
};

/*----------------------------------------------------------------------------*/
/*  TESTS                                                                     */
/*                                                                            */
/*----------------------------------------------------------------------------*/
TEST_F(LiteClientHSMTest, OstreeAndAppUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update both rootfs and add new app
  std::vector<AppEngine::App> apps{createApp("app-01")};
  auto new_target = createTarget(&apps);

  {
    EXPECT_CALL(*getAppEngine(), fetch).Times(1);

    // since the Target/app is not installed then no reason to check if the app is running
    EXPECT_CALL(*getAppEngine(), isRunning).Times(0);

    // Just install no need too call run
    EXPECT_CALL(*getAppEngine(), install).Times(1);
    EXPECT_CALL(*getAppEngine(), run).Times(0);

    // update to the latest version
    update(*client, getInitialTarget(), new_target);
  }

  {
    reboot(client);
    ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
    checkHeaders(*client, new_target);
  }
}

TEST_F(LiteClientHSMTest, AppUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target that just adds a new an app
  auto new_target = createAppTarget({createApp("app-01")});

  // update to the latest version
  EXPECT_CALL(*getAppEngine(), fetch).Times(1);

  // since the Target/app is not installed then no reason to check if the app is running
  EXPECT_CALL(*getAppEngine(), isRunning).Times(0);
  EXPECT_CALL(*getAppEngine(), install).Times(0);

  // just call run which includes install if necessary (no ostree update case)
  EXPECT_CALL(*getAppEngine(), run).Times(1);

  updateApps(*client, getInitialTarget(), new_target);
}

TEST_F(LiteClientHSMTest, OstreeUpdate) {
  // boot device
  auto client = createLiteClient();
  ASSERT_TRUE(targetsMatch(client->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*client, getInitialTarget(), new_target);

  // reboot device
  reboot(client);
  ASSERT_TRUE(targetsMatch(client->getCurrent(), new_target));
  checkHeaders(*client, new_target);
}

/*
 * main
 */
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
