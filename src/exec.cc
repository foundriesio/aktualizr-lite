#include "exec.h"

#include <boost/filesystem.hpp>
#include <boost/format.hpp>

#include "logging/logging.h"

#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <unistd.h>
#include <array>
#include <cstring>
#include <iostream>
#include <string>
#include <vector>

struct ProcessResult {
  std::string stdout_output;
  std::string stderr_output;
  int exit_code;
};

class Process {
 private:
  int stdout_pipe[2];
  int stderr_pipe[2];
  pid_t pid;

  // Set file descriptor to non-blocking mode
  bool setNonBlocking(int fd) {
    int flags = fcntl(fd, F_GETFL, 0);
    if (flags == -1) return false;
    return fcntl(fd, F_SETFL, flags | O_NONBLOCK) != -1;
  }

  // Read from both pipes simultaneously to avoid deadlock
  void readFromBothPipes(std::string& stdout_data, std::string& stderr_data, bool print_output) {
    std::array<char, 4096> buffer;
    fd_set read_fds;
    int max_fd = std::max(stdout_pipe[0], stderr_pipe[0]) + 1;
    bool stdout_open = true;
    bool stderr_open = true;

    // Set both pipes to non-blocking mode
    setNonBlocking(stdout_pipe[0]);
    setNonBlocking(stderr_pipe[0]);

    while (stdout_open || stderr_open) {
      FD_ZERO(&read_fds);

      if (stdout_open) FD_SET(stdout_pipe[0], &read_fds);
      if (stderr_open) FD_SET(stderr_pipe[0], &read_fds);

      int result = select(max_fd, &read_fds, nullptr, nullptr, NULL);
      if (result == -1) {
        LOG_ERROR << "Error in select: " << strerror(errno);
        throw std::runtime_error("exec: Error creating pipes");
      }

      // Read from stdout if data is available
      if (stdout_open && FD_ISSET(stdout_pipe[0], &read_fds)) {
        ssize_t bytes_read = read(stdout_pipe[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
          stdout_data.append(buffer.data(), bytes_read);
          if (print_output) {
            std::cout.write(buffer.data(), bytes_read);
            std::cout.flush();
          }
        } else if (bytes_read == 0) {
          stdout_open = false;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          stdout_open = false;
        }
      }

      // Read from stderr if data is available
      if (stderr_open && FD_ISSET(stderr_pipe[0], &read_fds)) {
        ssize_t bytes_read = read(stderr_pipe[0], buffer.data(), buffer.size());
        if (bytes_read > 0) {
          stderr_data.append(buffer.data(), bytes_read);
        } else if (bytes_read == 0) {
          stderr_open = false;
        } else if (errno != EAGAIN && errno != EWOULDBLOCK) {
          stderr_open = false;
        }
      }
    }
  }

 public:
  Process() : pid(-1) {
    stdout_pipe[0] = stdout_pipe[1] = -1;
    stderr_pipe[0] = stderr_pipe[1] = -1;
  }

  ~Process() { closeAllPipes(); }

  void closeAllPipes() {
    if (stdout_pipe[0] != -1) close(stdout_pipe[0]);
    if (stdout_pipe[1] != -1) close(stdout_pipe[1]);
    if (stderr_pipe[0] != -1) close(stderr_pipe[0]);
    if (stderr_pipe[1] != -1) close(stderr_pipe[1]);

    stdout_pipe[0] = stdout_pipe[1] = -1;
    stderr_pipe[0] = stderr_pipe[1] = -1;
  }

  ProcessResult execute(const std::string& command, bool print_output) {
    ProcessResult result;
    result.exit_code = -1;

    // Create pipes for stdout and stderr
    if (pipe(stdout_pipe) == -1 || pipe(stderr_pipe) == -1) {
      LOG_ERROR << "Error creating pipes: " << strerror(errno);
      throw std::runtime_error("exec: Error creating pipes");
    }

    // Setup file actions for posix_spawn
    posix_spawn_file_actions_t file_actions;
    posix_spawn_file_actions_init(&file_actions);

    // Redirect stdout to pipe
    posix_spawn_file_actions_addclose(&file_actions, stdout_pipe[0]);
    posix_spawn_file_actions_adddup2(&file_actions, stdout_pipe[1], STDOUT_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, stdout_pipe[1]);

    // Redirect stderr to pipe
    posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[0]);
    posix_spawn_file_actions_adddup2(&file_actions, stderr_pipe[1], STDERR_FILENO);
    posix_spawn_file_actions_addclose(&file_actions, stderr_pipe[1]);

    // Prepare arguments for shell execution
    char* argv[] = {const_cast<char*>("sh"), const_cast<char*>("-c"), const_cast<char*>(command.c_str()), nullptr};

    // Spawn the process
    int spawn_result = posix_spawn(&pid, "/bin/sh", &file_actions, nullptr, argv, environ);

    // Clean up file actions
    posix_spawn_file_actions_destroy(&file_actions);

    if (spawn_result != 0) {
      LOG_ERROR << "Error spawning process: " << strerror(spawn_result);
      closeAllPipes();
      throw std::runtime_error("exec: Error spawning process");
    }

    // Parent process - close write ends of pipes
    close(stdout_pipe[1]);
    stdout_pipe[1] = -1;
    close(stderr_pipe[1]);
    stderr_pipe[1] = -1;

    // Read from both pipes simultaneously to avoid deadlock
    readFromBothPipes(result.stdout_output, result.stderr_output, print_output);

    // Close read ends
    close(stdout_pipe[0]);
    stdout_pipe[0] = -1;
    close(stderr_pipe[0]);
    stderr_pipe[0] = -1;

    // Wait for child process to finish
    int status;
    waitpid(pid, &status, 0);

    if (WIFEXITED(status)) {
      result.exit_code = WEXITSTATUS(status);
    }

    return result;
  }
};

void exec(const std::string& cmd, const std::string& err_msg_prefix, const boost::filesystem::path& start_dir,
          std::string* output, const std::string& timeout, bool print_output) {
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
  if (!start_dir.empty()) {
    command = "cd " + start_dir.string() + " && " + command;
  }

  LOG_DEBUG << "Running: `" << command << "`";
  Process proc;
  auto result = proc.execute(command.c_str(), print_output);

  if (result.exit_code == 124) {
    // `timeout` command return code indicating that a timeout has occurred
    throw std::runtime_error("Timeout occurred while waiting for a child process completion");
  }
  LOG_DEBUG << "Command exited with code " << result.exit_code;

  if (result.exit_code != EXIT_SUCCESS) {
    throw ExecError(err_msg_prefix, cmd, result.stderr_output, result.exit_code);
  }
  if (output != nullptr) {
    *output = result.stdout_output;
  }
  if (result.stderr_output.size() > 0) {
    LOG_DEBUG << "Command stderr: " << result.stderr_output;
  }
}

void exec(const boost::format& cmd, const std::string& err_msg, const boost::filesystem::path& start_dir,
          std::string* output, const std::string& timeout, bool print_output) {
  exec(cmd.str(), err_msg, start_dir, output, timeout, print_output);
}
