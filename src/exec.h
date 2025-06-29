#ifndef AKTUALIZR_LITE_EXEC_H_
#define AKTUALIZR_LITE_EXEC_H_

#include <boost/asio/io_context.hpp>
#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include <boost/version.hpp>

#if BOOST_VERSION >= 108800
#include <boost/process/v1/args.hpp>
#include <boost/process/v1/async.hpp>
#include <boost/process/v1/async_system.hpp>
#include <boost/process/v1/child.hpp>
#include <boost/process/v1/cmd.hpp>
#include <boost/process/v1/env.hpp>
#include <boost/process/v1/environment.hpp>
#include <boost/process/v1/error.hpp>
#include <boost/process/v1/exe.hpp>
#include <boost/process/v1/group.hpp>
#include <boost/process/v1/handles.hpp>
#include <boost/process/v1/io.hpp>
#include <boost/process/v1/pipe.hpp>
#include <boost/process/v1/search_path.hpp>
#include <boost/process/v1/shell.hpp>
#include <boost/process/v1/spawn.hpp>
#include <boost/process/v1/start_dir.hpp>
#include <boost/process/v1/system.hpp>
namespace bp = boost::process::v1;
#else
#include <boost/process.hpp>
namespace bp = boost::process;
#endif
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
static void exec(const std::string& cmd, const std::string& err_msg_prefix, Args&&... args) {
  // Implementation is based on test_utils.cc:Process::spawn that has been proven over time
  std::future<std::string> err_output;
  std::future<int> child_process_exit_code;
  boost::asio::io_context io_context;

  try {
    LOG_DEBUG << "Running: `" << cmd << "`";
    bp::child child_process(cmd, bp::std_err > err_output, bp::on_exit = child_process_exit_code, io_context,
                            std::forward<Args>(args)...);

    io_context.run();

    bool wait_successfull = child_process.wait_for(std::chrono::seconds(900));
    if (!wait_successfull) {
      throw std::runtime_error("Timeout occurred while waiting for a child process completion");
    }

  } catch (const std::exception& exc) {
    throw std::runtime_error("Failed to spawn process " + cmd + " exited with an error: " + exc.what());
  }

  const auto exit_code{child_process_exit_code.get()};
  LOG_DEBUG << "Command exited with code " << exit_code;

  if (exit_code != EXIT_SUCCESS) {
    const auto err_msg{err_output.get()};
    throw ExecError(err_msg_prefix, cmd, err_msg, exit_code);
  }
}

template <typename... Args>
void exec(const boost::format& cmd, const std::string& err_msg, Args&&... args) {
  exec(cmd.str(), err_msg, std::forward<Args>(args)...);
}

#endif  // AKTUALIZR_LITE_EXEC_H_
