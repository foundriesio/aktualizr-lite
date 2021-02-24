#include <iostream>
#include <unordered_map>

#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "crypto/keymanager.h"
#include "helpers.h"
#include "libaktualizr/config.h"

#include "target.h"
#include "utilities/aktualizr_version.h"

namespace bpo = boost::program_options;

static int status_main(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;
  auto target = client.getCurrent(true);

  try {
    LOG_INFO << "Device UUID: " << client.getDeviceID();
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get a device UUID: " << exc.what();
  }

  bool fetch_device_info_res;
  Json::Value device_info;
  std::tie(fetch_device_info_res, device_info) = client.getDeviceInfo();

  if (fetch_device_info_res) {
    if (!device_info.empty()) {
      LOG_INFO << "Device name: " << device_info["Name"].asString();
    } else {
      LOG_WARNING << "Failed to get a device name from a device info: " << device_info;
    }
  } else {
    LOG_WARNING << "Failed to get a device info: " << device_info.get("err", "unknown error");
  }

  if (target.MatchTarget(Uptane::Target::Unknown())) {
    LOG_INFO << "No active deployment found";
  } else {
    auto name = target.filename();
    if (target.custom_version().length() > 0) {
      name = target.custom_version();
    }
    client.logTarget("Active image is: ", target);
  }
  return 0;
}

static int list_main(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;

  LOG_INFO << "Refreshing Targets metadata";
  client.refreshMetadata();
  boost::container::flat_map<int, Uptane::Target> sorted_targets = client.getTargets();

  LOG_INFO << "Available updates: ";
  for (auto& pair : sorted_targets) {
    client.logTarget("", pair.second);
  }
  return 0;
}

static int update_main(LiteClient& client, const bpo::variables_map& variables_map) {
  std::string version("latest");

  if (variables_map.count("update-name") > 0) {
    version = variables_map["update-name"].as<std::string>();
  }

  data::ResultCode::Numeric rc = client.update(version, true);
  return (rc == data::ResultCode::Numeric::kNeedCompletion || rc == data::ResultCode::Numeric::kOk) ? EXIT_SUCCESS
                                                                                                    : EXIT_FAILURE;
}

static int daemon_main(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;
  client.reportStatus();

  auto interval = client.updateInterval();
  while (true) {
    const auto& current = client.getCurrent(true);
    LOG_INFO << "Active Target: " << current.filename() << ", sha256: " << current.sha256Hash();

    try {
      data::ResultCode::Numeric rc = client.update();
      if (rc == data::ResultCode::Numeric::kNeedCompletion) {
        // no point to continue running TUF cycle (check for update, download, install)
        // since reboot is required to apply/finalize the currently installed update (aka pending update)
        break;
      }

      if (rc == data::ResultCode::Numeric::kOk || rc == data::ResultCode::Numeric::kAlreadyProcessed) {
        LOG_INFO << "Device is up-to-date";
      } else {
        LOG_ERROR << "Failed to update or sync the latest Target: " + data::ResultCode(rc).toString();
      }
    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to find or update Target: " << exc.what();
    }

    LOG_INFO << "Wait " << interval << " seconds for the next update cycle...";
    std::this_thread::sleep_for(std::chrono::seconds(interval));
  }  // while true

  return EXIT_SUCCESS;
}

static const std::unordered_map<std::string, int (*)(LiteClient&, const bpo::variables_map&)> commands = {
    {"daemon", daemon_main},
    {"update", update_main},
    {"list", list_main},
    {"status", status_main},
};

void check_info_options(const bpo::options_description& description, const bpo::variables_map& vm) {
  if (vm.count("help") != 0 || (vm.count("command") == 0 && vm.count("version") == 0)) {
    std::cout << description << '\n';
    exit(EXIT_SUCCESS);
  }
  if (vm.count("version") != 0) {
    std::cout << "Current aktualizr version is: " << aktualizr_version() << "\n";
    exit(EXIT_SUCCESS);
  }
}

bpo::variables_map parse_options(int argc, char** argv) {
  std::string subs("Command to execute: ");
  for (const auto& cmd : commands) {
    static int indx = 0;
    if (indx != 0) {
      subs += ", ";
    }
    subs += cmd.first;
    ++indx;
  }
  bpo::options_description description("aktualizr-lite command line options");

  // clang-format off
  // Try to keep these options in the same order as Config::updateFromCommandLine().
  // The first three are commandline only.
  description.add_options()
      ("help,h", "print usage")
      ("version,v", "Current aktualizr version")
      ("config,c", bpo::value<std::vector<boost::filesystem::path> >()->composing(), "configuration file or directory")
      ("loglevel", bpo::value<int>(), "set log level 0-5 (trace, debug, info, warning, error, fatal)")
      ("repo-server", bpo::value<std::string>(), "URL of the Uptane repo repository")
      ("ostree-server", bpo::value<std::string>(), "URL of the Ostree repository")
      ("primary-ecu-hardware-id", bpo::value<std::string>(), "hardware ID of primary ecu")
      ("update-name", bpo::value<std::string>(), "optional name of the update when running \"update\". default=latest")
#ifdef ALLOW_MANUAL_ROLLBACK
      ("clear-installed-versions", "DANGER - clear the history of installed updates before applying the given update. This is handy when doing test/debug and you need to rollback to an old version manually.")
#endif
      ("interval", bpo::value<uint64_t>(), "Override uptane.polling_secs interval to poll for update when in daemon mode.")
      ("update-lockfile", bpo::value<boost::filesystem::path>(), "If provided, an flock(2) is applied to this file before performing an update in daemon mode")
      ("download-lockfile", bpo::value<boost::filesystem::path>(), "If provided, an flock(2) is applied to this file before downloading an update in daemon mode")
      ("command", bpo::value<std::string>(), subs.c_str());
  // clang-format on

  // consider the first positional argument as the aktualizr run mode
  bpo::positional_options_description pos;
  pos.add("command", 1);

  bpo::variables_map vm;
  std::vector<std::string> unregistered_options;
  try {
    bpo::basic_parsed_options<char> parsed_options =
        bpo::command_line_parser(argc, argv).options(description).positional(pos).allow_unregistered().run();
    bpo::store(parsed_options, vm);
    check_info_options(description, vm);
    bpo::notify(vm);
    unregistered_options = bpo::collect_unrecognized(parsed_options.options, bpo::exclude_positional);
    if (vm.count("help") == 0 && !unregistered_options.empty()) {
      std::cout << description << "\n";
      exit(EXIT_FAILURE);
    }
  } catch (const bpo::required_option& ex) {
    // print the error and append the default commandline option description
    std::cout << ex.what() << std::endl << description;
    exit(EXIT_FAILURE);
  } catch (const bpo::error& ex) {
    check_info_options(description, vm);

    // log boost error
    LOG_ERROR << "boost command line option error: " << ex.what();

    // print the error message to the standard output too, as the user provided
    // a non-supported commandline option
    std::cout << ex.what() << '\n';

    // set the returnValue, thereby ctest will recognize
    // that something went wrong
    exit(EXIT_FAILURE);
  }

  return vm;
}

int main(int argc, char* argv[]) {
  logger_init(isatty(1) == 1);
  logger_set_threshold(boost::log::trivial::info);

  bpo::variables_map commandline_map = parse_options(argc, argv);

  int ret_val = EXIT_FAILURE;
  try {
    if (geteuid() != 0) {
      LOG_WARNING << "\033[31mRunning as non-root and may not work as expected!\033[0m\n";
    }

    Config config(commandline_map);
    config.storage.uptane_metadata_path = utils::BasedPath(config.storage.path / "metadata");
    config.telemetry.report_network = !config.tls.server.empty();
    config.telemetry.report_config = !config.tls.server.empty();

    LOG_DEBUG << "Current directory: " << boost::filesystem::current_path().string();

    std::string cmd = commandline_map["command"].as<std::string>();
    auto cmd_to_run = commands.find(cmd);
    if (cmd_to_run == commands.end()) {
      throw bpo::invalid_option_value("Unsupported command: " + cmd);
    }

    std::pair<bool, std::string> is_reboot_required{false, ""};
    {
      LOG_DEBUG << "Running " << (*cmd_to_run).first;
      LiteClient client(config, &commandline_map);
      ret_val = (*cmd_to_run).second(client, commandline_map);
      if (cmd == "daemon") {
        is_reboot_required = client.isRebootRequired();
      }
    }

    if (ret_val == EXIT_SUCCESS && is_reboot_required.first) {
      LOG_INFO << "Device is going to reboot (" << is_reboot_required.second << ")";
      if (setuid(0) != 0) {
        LOG_ERROR << "Failed to set/verify a root user so cannot reboot system programmatically";
      } else {
        sync();
        // let's try to reboot the system, if it fails we just throw an exception and exit the process
        if (std::system(is_reboot_required.second.c_str()) != 0) {
          throw std::runtime_error("Failed to execute the reboot command: " + config.bootloader.reboot_command);
        }
      }
    }

  } catch (const std::exception& ex) {
    LOG_ERROR << ex.what();
    ret_val = EXIT_FAILURE;
  }

  return ret_val;
}
