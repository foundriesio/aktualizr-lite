#ifndef AKLITE_APPS_CMD_H
#define AKLITE_APPS_CMD_H

#include <string>

#include <boost/algorithm/string/classification.hpp>
#include <boost/algorithm/string/split.hpp>
#include <boost/program_options.hpp>
#include "logging/logging.h"

namespace po = boost::program_options;

namespace apps {
namespace aklite_apps {

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

class ListCmd : public Cmd {
 public:
  ListCmd() : Cmd("ls", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "store-root", po::value<std::string>()->default_value("/var/sota/reset-apps"), "Image store root folder")(
        "wide,w", po::bool_switch()->default_value(false), "Print App URI");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      return listApps(vm["store-root"].as<std::string>(), vm["wide"].as<bool>());
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to list Apps: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  static int listApps(const std::string& store_root, bool wide);

  po::options_description _options;
};

class RunCmd : public Cmd {
 public:
  RunCmd() : Cmd("run", _options) {
    _options.add_options()("help,h", "print usage")("log-level", po::value<int>()->default_value(2),
                                                    "set log level 0-5 (trace, debug, info, warning, error, fatal)")(
        "apps", po::value<std::string>()->default_value(""),
        "Comma separated list of Apps to run, by default all Apps are started")(
        "docker-host", po::value<std::string>()->default_value("unix:///var/run/docker.sock"),
        "Socket that a docker deamon listens to")(
        "store-root", po::value<std::string>()->default_value("/var/sota/reset-apps"), "Image store root folder")(
        "compose-root", po::value<std::string>()->default_value("/var/sota/compose-apps"), "Compose Apps root folder")(
        "docker-root", po::value<std::string>()->default_value("/var/lib/docker"), "Docker data root folder")(
        "client", po::value<std::string>()->default_value("/usr/sbin/skopeo"), "A client to copy images")(
        "compose-client", po::value<std::string>()->default_value("/usr/bin/docker compose "),
        "A client to manage compose apps");
  }

  int operator()(const po::variables_map& vm) const override {
    try {
      std::vector<std::string> apps;
      if (!vm["apps"].as<std::string>().empty()) {
        boost::split(apps, vm["apps"].as<std::string>(), boost::is_any_of(", "), boost::token_compress_on);
      }

      return runApps(apps, vm["docker-host"].as<std::string>(), vm["store-root"].as<std::string>(),
                     vm["compose-root"].as<std::string>(), vm["docker-root"].as<std::string>(),
                     vm["client"].as<std::string>(), vm["compose-client"].as<std::string>());
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to run preloaded Apps: " << exc.what();
      return EXIT_FAILURE;
    }
  }

 private:
  static int runApps(const std::vector<std::string>& shortlist, const std::string& docker_host,
                     const std::string& store_root, const std::string& compose_root, const std::string& docker_root,
                     const std::string& client, const std::string& compose_client);

  po::options_description _options;
};

}  // namespace aklite_apps
}  // namespace apps

#endif  // AKLITE_APPS_FIOAPP_CMD_H
