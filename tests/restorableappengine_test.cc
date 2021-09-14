#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/format.hpp>
#include <boost/process.hpp>

#include "crypto/crypto.h"
#include "logging/logging.h"

#include "docker/composeappengine.h"
#include "docker/restorableappengine.h"

#include "fixtures/composeappenginetest.cc"

class RestorableAppEngineTest : public fixtures::AppEngineTest {
 protected:
  void SetUp() override {
    fixtures::AppEngineTest::SetUp();
    app_engine = std::make_shared<Docker::RestorableAppEngine>();
  }
};

TEST_F(RestorableAppEngineTest, InitDeinit) {}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  return RUN_ALL_TESTS();
}
