#include <gtest/gtest.h>

#include "exec.h"
#include "utilities/utils.h"

TEST(Exec, SuccessfulExec) {
  TemporaryDirectory test_dir;
  const auto test_file{test_dir / "test-file"};
  exec("touch " + test_file.string(), "touch failed", test_dir.Path());
  ASSERT_TRUE(boost::filesystem::exists(test_file));
}

TEST(Exec, FailedExec) {
  const auto executable{"non-existing-executable"};
  try {
    exec(executable, "");
  } catch (const std::exception& exc) {
    const std::string err_msg{exc.what()};

    ASSERT_NE(err_msg.find("not found"), std::string::npos) << "Actual error message: " + err_msg;
    ASSERT_NE(err_msg.find(executable), std::string::npos) << "Actual error message: " + err_msg;
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
    // The exact wording ("unrecognized option" vs "unexpected argument") differs between GNU
    // coreutils and other `ls` implementations (e.g. Ubuntu 26.10's uutils-based `ls`), so just
    // check that the offending option was echoed back, which both report.
    ASSERT_NE(err_msg.find(bad_option), std::string::npos) << "Actual error message: " + err_msg;
  }
}

TEST(Exec, ExecTimeout) {
  const std::string cmd{"sleep 10"};

  try {
    exec(cmd, "", "", nullptr, "2s", false);
  } catch (const std::runtime_error& exc) {
    const std::string err_msg{exc.what()};

    ASSERT_NE(err_msg.find("Timeout"), std::string::npos) << "Actual error message: " + err_msg;
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
