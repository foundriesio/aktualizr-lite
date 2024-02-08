#ifndef AKLITE_CUSTOM_CMD_H
#define AKLITE_CUSTOM_CMD_H

#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/log/core.hpp>
#include <boost/log/trivial.hpp>
#include <boost/log/expressions.hpp>

#include "cli.h"

#define LOG_INFO    BOOST_LOG_TRIVIAL(info)
#define LOG_WARNING BOOST_LOG_TRIVIAL(warning)
#define LOG_ERROR   BOOST_LOG_TRIVIAL(error)

namespace po = boost::program_options;

class Cmd {
 public:
  using Ptr = std::shared_ptr<Cmd>;
  const std::string& name() const { return _name; }
  const po::options_description& options() const { return _options; }
  const std::vector<std::string>& pos_options() const { return _pos_options; }

  virtual int operator()(const po::variables_map& vm) const = 0;

 protected:
  Cmd(std::string name, const po::options_description& options, std::vector<std::string>& pos_options) : _name{std::move(name)}, _options{options}, _pos_options{pos_options} {}

 private:
  const std::string _name;
  const po::options_description& _options;
  const std::vector<std::string>& _pos_options;
};

class CheckCmd : public Cmd {
 public:
  CheckCmd() : Cmd("check", _options, _pos_options) {
    _options.add_options()("help,h", "print usage")(
        "src-dir,s", po::value<std::string>()->default_value(std::string("")), "Directory that contains an update");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return cmd_check(vm["src-dir"].as<std::string>());
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to check the update source directory: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  po::options_description _options;
  std::vector<std::string> _pos_options;
};

class InstallCmd : public Cmd {
 public:
  InstallCmd() : Cmd("install", _options, _pos_options) {
    _options.add_options()("help,h", "print usage")(
        "src-dir,s", po::value<std::string>()->default_value(std::string("")), "Directory that contains an update")(
        "target,t", po::value<std::string>()->default_value(std::string("")), "Target name");
    _pos_options.emplace_back("target");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return cmd_install(vm["target"].as<std::string>(), vm["src-dir"].as<std::string>());
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to install target: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  po::options_description _options;
  std::vector<std::string> _pos_options;
  bool force_downgrade{false};
};

class RunCmd : public Cmd {
 public:
  RunCmd() : Cmd("run", _options, _pos_options) {
    _options.add_options()("help,h", "print usage");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return cmd_run();
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to list Apps: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  po::options_description _options;
  std::vector<std::string> _pos_options;
};

class PullCmd : public Cmd {
 public:
  PullCmd() : Cmd("pull", _options, _pos_options) {
    _options.add_options()("help,h", "print usage")(
        "src-dir,s", po::value<std::string>()->default_value(std::string("")), "Directory that contains an update")(
        "target,t", po::value<std::string>()->default_value(std::string("")), "Target name");
    _pos_options.emplace_back("target");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return cmd_pull(vm["target"].as<std::string>(), vm["src-dir"].as<std::string>());
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to get current status information: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  po::options_description _options;
  std::vector<std::string> _pos_options;
};

class DaemonCmd : public Cmd {
 public:
  DaemonCmd() : Cmd("daemon", _options, _pos_options) {
    _options.add_options()("help,h", "print usage")(
      "src-dir,s", po::value<std::string>()->default_value(std::string("")), "Directory that contains an update");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return cmd_daemon(vm["src-dir"].as<std::string>());
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to get current status information: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  po::options_description _options;
  std::vector<std::string> _pos_options;
};

#endif  // AKLITE_CUSTOM_CMD_H
