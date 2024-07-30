#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "uptane_generator/image_repo.h"
#include "utilities/utils.h"

#include "aktualizr-lite/api.h"
#include "composeappmanager.h"
#include "daemon.h"
#include "liteclient.h"

#include "fixtures/liteclienttest.cc"

using ::testing::NiceMock;

class DaemonTest : public fixtures::ClientTest {
 protected:
  std::shared_ptr<fixtures::LiteClientMock> createLiteClient(
      InitialVersion initial_version = InitialVersion::kOn,
      boost::optional<std::vector<std::string>> apps = boost::none, bool finalize = true) override {
    app_engine_mock_ = std::make_shared<NiceMock<fixtures::MockAppEngine>>();
    lite_client_ = ClientTest::createLiteClient(app_engine_mock_, initial_version, apps);
    return lite_client_;
  }

 private:
  std::shared_ptr<NiceMock<fixtures::MockAppEngine>> app_engine_mock_;
  std::shared_ptr<fixtures::LiteClientMock> lite_client_;
  std::string pacman_type_;
};

TEST_F(DaemonTest, MainDaemonOstreeInstall) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();

  // Run one iteration of daemon code: install should be successful, requiring a reboot
  auto daemon_ret = run_daemon(*liteclient, 100, true, false);
  ASSERT_EQ(daemon_ret, EXIT_SUCCESS);

  // Trying to run daemon again before rebooting leads to an error, and the original target still running
  daemon_ret = run_daemon(*liteclient, 100, true, false);
  ASSERT_EQ(daemon_ret, EXIT_FAILURE);
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  reboot(liteclient);

  // After reboot, running the daemon again finishes the installation, the function returns successfully,
  // and the new target becomes the current one
  daemon_ret = run_daemon(*liteclient, 100, true, false);
  ASSERT_EQ(daemon_ret, EXIT_SUCCESS);
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), new_target));
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
