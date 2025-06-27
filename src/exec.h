#ifndef AKTUALIZR_LITE_EXEC_H_
#define AKTUALIZR_LITE_EXEC_H_

#include <boost/asio/io_context.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include "logging/logging.h"

struct ExecError : std::runtime_error {
  ExecError(const std::string& msg_prefix, const std::string& cmd, const std::string& err_msg, int exit_code)
      : std::runtime_error(msg_prefix + "\n\tcmd: " + cmd + "\n\terr: " + err_msg),
        ExitCode{exit_code},
        StdErr{err_msg} {}
  const int ExitCode;
  const std::string StdErr;
};

template <typename... Args>
static void exec(const std::string& cmd, const std::string& err_msg_prefix,
                 const boost::filesystem::path& start_dir = "", std::string* output = nullptr) {
  std::string command = "timeout 900 " + cmd + " 2>&1";
  if (!start_dir.empty()) {
    command = "cd " + start_dir.string() + " && " + command;
  }

  LOG_DEBUG << "Running: `" << command << "`";
  FILE* pipe = popen(command.c_str(), "r");
  if (!pipe) {
    throw std::runtime_error("exec: popen() failed!");
  }
  std::string result;
  try {
    std::array<char, 128> buffer_array = {};
    char* buffer = buffer_array.data();
    while (std::fgets(buffer, sizeof(buffer_array), pipe) != nullptr) {
      result += buffer;
    }

    if (output != nullptr) {
      *output = result;
    }
  } catch (const std::exception& e) {
    pclose(pipe);
    throw std::runtime_error(std::string("exec: ") + e.what());
  }

  int status = pclose(pipe);
  if (status == -1) {
    throw std::runtime_error("exec: pclose() failed!");
  }

  int exit_code = WEXITSTATUS(status);

  if (exit_code == 124) {
    // `timeout` command return code indicating that a timeout has occured
    throw std::runtime_error("Timeout occurred while waiting for a child process completion");
  }
  LOG_DEBUG << "Command exited with code " << exit_code;

  if (exit_code != EXIT_SUCCESS) {
    throw ExecError(err_msg_prefix, cmd, result, exit_code);
  }
}

template <typename... Args>
void exec(const boost::format& cmd, const std::string& err_msg, const boost::filesystem::path& start_dir = "",
          std::string* output = nullptr) {
  exec(cmd.str(), err_msg, start_dir, output);
}

#endif  // AKTUALIZR_LITE_EXEC_H_
