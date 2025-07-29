#ifndef AKTUALIZR_LITE_EXEC_H_
#define AKTUALIZR_LITE_EXEC_H_

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

struct ExecError : std::runtime_error {
  ExecError(const std::string& msg_prefix, const std::string& cmd, const std::string& err_msg, int exit_code)
      : std::runtime_error(msg_prefix + "\n\tcmd: " + cmd + "\n\terr: " + err_msg),
        ExitCode{exit_code},
        StdErr{err_msg} {}
  const int ExitCode;
  const std::string StdErr;
};

void exec(const std::string& cmd, const std::string& err_msg_prefix, const boost::filesystem::path& start_dir = "",
          std::string* output = nullptr, const std::string& timeout = "900s", bool print_output = false);

void exec(const boost::format& cmd, const std::string& err_msg, const boost::filesystem::path& start_dir = "",
          std::string* output = nullptr, const std::string& timeout = "900s", bool print_output = false);

#endif  // AKTUALIZR_LITE_EXEC_H_
