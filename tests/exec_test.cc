#include <gtest/gtest.h>

#include "exec.h"
#include "utilities/utils.h"

TEST(Exec, SuccessfulExec) {
  TemporaryDirectory test_dir;
  const auto test_file{test_dir / "test-file"};
  exec("touch " + test_file.string(), "touch failed", bp::start_dir = test_dir.Path());
  ASSERT_TRUE(boost::filesystem::exists(test_file));
}

TEST(Exec, FailedExec) {
  const auto executable{"non-existing-executable"};
  try {
    exec(executable, "");
  } catch (const std::exception& exc) {
    const std::string err_msg{exc.what()};

    ASSERT_EQ(err_msg.find("Failed to spawn process"), 0);
    ASSERT_NE(err_msg.find(executable), std::string::npos) << "Actual error message: " + err_msg;
    ;
    ASSERT_NE(err_msg.find("No such file or directory"), std::string::npos) << "Actual error message: " + err_msg;
    ;
  }
}

TEST(Exec, SuccessfulExecFailedExecutable) {
  const std::string executable{"ls"};
  const std::string bad_option{"--foobar"};
  const std::string err_msg_prefix{executable + " failed"};

  try {
    exec(executable + " " + bad_option, err_msg_prefix);
  } catch (const std::exception& exc) {
    const std::string err_msg{exc.what()};

    ASSERT_EQ(err_msg.find(err_msg_prefix), 0);
    ASSERT_NE(err_msg.find("unrecognized option \'" + bad_option + "\'"), std::string::npos)
        << "Actual error message: " + err_msg;
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
