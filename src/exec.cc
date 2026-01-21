#include "exec.h"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include "logging/logging.h"

void exec(const std::string& cmd, const std::string& err_msg_prefix, const boost::filesystem::path& start_dir,
          std::string* output, const std::string& timeout, bool print_output, bool ignore_stderr) {
  std::string command;

  if (print_output) {
    setvbuf(stdout, NULL, _IOLBF, 0);
  }

  if (print_output && isatty(STDOUT_FILENO)) {
    command = "PARENT_HAS_TTY=1 ";
  }

  if (!timeout.empty()) {
    command += "timeout " + timeout + " ";
  }

  command += cmd;

  if (!ignore_stderr) {
    command + " 2>&1";
  }

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
      if (print_output) {
        fputs(buffer, stdout);
      }
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

void exec(const boost::format& cmd, const std::string& err_msg, const boost::filesystem::path& start_dir,
          std::string* output, const std::string& timeout, bool print_output, bool ignore_stderr) {
  exec(cmd.str(), err_msg, start_dir, output, timeout, print_output, ignore_stderr);
}
