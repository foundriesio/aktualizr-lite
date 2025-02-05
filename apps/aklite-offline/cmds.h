#ifndef AKLITE_OFFLINE_CMD_H
#define AKLITE_OFFLINE_CMD_H

#include <iostream>
#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

namespace po = boost::program_options;

namespace apps {
namespace aklite_offline {

class Cmd {
 public:
  using Ptr = std::shared_ptr<Cmd>;
  const std::string& name() const { return _name; }
  const std::string& description() const { return _description; }
  const po::options_description& options() const { return _options; }

  virtual int operator()(const po::variables_map& vm) const = 0;

 protected:
  Cmd(std::string name, std::string description, const po::options_description& options)
      : _name{std::move(name)}, _description{std::move(description)}, _options{options} {}

 private:
  const std::string _name;
  const std::string _description;
  const po::options_description& _options;
};

class CheckCmd : public Cmd {
 public:
  CheckCmd()
      : Cmd("check",
            "Update the device TUF metadata by fetching and validating the offline bundle's metadata. The list of "
            "available targets is printed",
            _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory")(
        "src-dir,s", po::value<boost::filesystem::path>()->required(), "Directory that contains an update");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return checkSrcDir(vm, boost::filesystem::canonical(vm["src-dir"].as<boost::filesystem::path>()));
    } catch (const std::exception& exc) {
      std::cerr << "Failed to check the update source directory: " << exc.what() << std::endl;
      return EXIT_FAILURE;
    }
  }

 private:
  int checkSrcDir(const po::variables_map& vm, const boost::filesystem::path& src_dir) const;

 private:
  po::options_description _options;
};

class InstallCmd : public Cmd {
 public:
  InstallCmd()
      : Cmd("install",
            "Install the selected target. If no target name is specified, the highest version in the bundle will "
            "be used",
            _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory")(
        "src-dir,s", po::value<boost::filesystem::path>()->required(), "Directory that contains an update")(
        "force,f", po::bool_switch(&force_downgrade), "Force downgrade")(
        "target,t", po::value<std::string>()->default_value(std::string("")), "Target name");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return installUpdate(vm, boost::filesystem::canonical(vm["src-dir"].as<boost::filesystem::path>()),
                           vm["target"].as<std::string>(), force_downgrade);
    } catch (const std::exception& exc) {
      std::cerr << "Failed to install offline update; src-dir: " << vm["src-dir"].as<boost::filesystem::path>().string()
                << ", err: " << exc.what() << "\n";
      return EXIT_FAILURE;
    }
  }

 private:
  int installUpdate(const po::variables_map& vm, const boost::filesystem::path& src_dir, const std::string& target_name,
                    bool force_downgrade) const;

 private:
  po::options_description _options;
  bool force_downgrade{false};
};

class RunCmd : public Cmd {
 public:
  RunCmd() : Cmd("run", "Finalize the installation of a target, starting the updated apps", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return runUpdate(vm);
    } catch (const std::exception& exc) {
      std::cerr << "Failed to finalize the update and start updated Apps; err: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  int runUpdate(const po::variables_map& vm) const;

 private:
  po::options_description _options;
};

class CurrentCmd : public Cmd {
 public:
  CurrentCmd() : Cmd("current", "Show information about the currently running target", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "config,c", po::value<std::vector<boost::filesystem::path>>()->composing(), "Configuration file or directory");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return current(vm);
    } catch (const std::exception& exc) {
      std::cerr << "Failed to get current status information: " << exc.what() << std::endl;
      return EXIT_FAILURE;
    }
  }

 private:
  int current(const po::variables_map& vm) const;

 private:
  po::options_description _options;
};

}  // namespace aklite_offline
}  // namespace apps

#endif  // AKLITE_OFFLINE_CMD_H
