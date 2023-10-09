#ifndef AKLITE_OFFLINE_CMD_H
#define AKLITE_OFFLINE_CMD_H

#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include "libaktualizr/config.h"
#include "logging/logging.h"

namespace po = boost::program_options;

namespace apps {
namespace aklite_offline {

class Cmd {
 public:
  using Ptr = std::shared_ptr<Cmd>;
  const std::string& name() const { return _name; }
  const po::options_description& options() const { return _options; }

  virtual int operator()(const po::variables_map& vm) const = 0;

 protected:
  Cmd(std::string name, const po::options_description& options) : _name{std::move(name)}, _options{options} {}

 private:
  const std::string _name;
  const po::options_description& _options;
};

class CheckCmd : public Cmd {
 public:
  CheckCmd() : Cmd("check", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory")(
        "src-dir,s", po::value<boost::filesystem::path>()->required(), "Directory that contains an update");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      Config cfg_in{vm};
      return checkSrcDir(cfg_in, boost::filesystem::canonical(vm["src-dir"].as<boost::filesystem::path>()));
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to check the update source directory: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  int checkSrcDir(const Config& cfg_in, const boost::filesystem::path& src_dir) const;

 private:
  po::options_description _options;
};

class InstallCmd : public Cmd {
 public:
  InstallCmd() : Cmd("install", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory")(
        "src-dir,s", po::value<boost::filesystem::path>()->required(), "Directory that contains an update");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      Config cfg_in{vm};
      return installUpdate(cfg_in, boost::filesystem::canonical(vm["src-dir"].as<boost::filesystem::path>()));
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to list Apps: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  int installUpdate(const Config& cfg_in, const boost::filesystem::path& src_dir) const;

 private:
  po::options_description _options;
};

class RunCmd : public Cmd {
 public:
  RunCmd() : Cmd("run", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      Config cfg_in{vm};
      return runUpdate(cfg_in);
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to list Apps: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  int runUpdate(const Config& cfg_in) const;

 private:
  po::options_description _options;
};

class CurrentCmd : public Cmd {
 public:
  CurrentCmd() : Cmd("current", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      Config cfg_in{vm};
      return current(cfg_in);
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to get current status information: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  int current(const Config& cfg_in) const;

 private:
  po::options_description _options;
};

}  // namespace aklite_offline
}  // namespace apps

#endif  // AKLITE_OFFLINE_CMD_H
