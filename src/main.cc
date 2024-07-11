#include <sys/file.h>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <thread>
#include <unordered_map>

#include <boost/algorithm/string/join.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/program_options.hpp>

#include "aktualizr-lite/aklite_client_ext.h"
#include "aktualizr-lite/api.h"
#include "aktualizr-lite/cli/cli.h"
#include "crypto/keymanager.h"
#include "daemon.h"
#include "helpers.h"
#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "logging/logging.h"
#include "storage/invstorage.h"
#include "target.h"
#include "utilities/aktualizr_version.h"

namespace bpo = boost::program_options;

static int status_finalize(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;
  return client.finalizeInstall() ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int status_main(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;
  auto target = client.getCurrent();

  try {
    LOG_INFO << "Device UUID: " << client.getDeviceID();
  } catch (const std::exception& exc) {
    LOG_WARNING << "Failed to get a device UUID: " << exc.what();
  }

  const auto http_res = client.http_client->get(client.config.tls.server + "/device", HttpInterface::kNoLimit);
  if (http_res.isOk()) {
    const Json::Value device_info = http_res.getJson();
    if (!device_info.empty()) {
      LOG_INFO << "Device name: " << device_info["Name"].asString();
    } else {
      LOG_WARNING << "Failed to get a device name from a device info: " << device_info;
    }
  } else {
    LOG_WARNING << "Failed to get a device info: " << http_res.getStatusStr();
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
  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClient akclient(client_ptr, false, true);

  auto status = aklite::cli::CheckIn(akclient, nullptr);
  return aklite::cli::IsSuccessCode(status) ? EXIT_SUCCESS : EXIT_FAILURE;
}

static int daemon_main(LiteClient& client, const bpo::variables_map& variables_map) {
  uint64_t interval = client.config.uptane.polling_sec;
  if (variables_map.count("interval") > 0) {
    interval = variables_map["interval"].as<uint64_t>();
  }

  bool return_on_sleep = std::getenv("AKLITE_TEST_RETURN_ON_SLEEP") != nullptr;

  return run_daemon(client, interval, return_on_sleep, true);
}

static void get_target_id(const bpo::variables_map& params, int& version, std::string& name) {
  name = "";
  version = -1;
  if (params.count("update-name") > 0) {
    const auto version_str = params["update-name"].as<std::string>();
    try {
      version = std::stoi(version_str);
    } catch (const std::invalid_argument& exc) {
      LOG_DEBUG << "Failed to convert the input target version to integer, consider it as a name of Target: "
                << exc.what();
      name = version_str;
    }
  }
}

static int install(LiteClient& client, const bpo::variables_map& params, aklite::cli::PullMode pull_mode,
                   aklite::cli::CheckMode check_mode) {
  int version;
  std::string target_name;
  get_target_id(params, version, target_name);

  std::string install_mode;
  if (params.count("install-mode") > 0) {
    install_mode = params.at("install-mode").as<std::string>();
  }

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClientExt akclient{client_ptr, false, true};

  const static std::unordered_map<std::string, InstallMode> str2Mode = {{"delay-app-install", InstallMode::OstreeOnly}};
  InstallMode mode{InstallMode::All};
  if (!install_mode.empty()) {
    if (str2Mode.count(install_mode) == 0) {
      LOG_WARNING << "Unsupported installation mode: " << install_mode << "; falling back to the default install mode";
    } else {
      mode = str2Mode.at(install_mode);
    }
  }
  return static_cast<int>(
      aklite::cli::Install(akclient, version, target_name, mode, true, nullptr, pull_mode, check_mode));
}

// CheckIn, Pull and Install
static int cli_update(LiteClient& client, const bpo::variables_map& params) {
  return install(client, params, aklite::cli::PullMode::All, aklite::cli::CheckMode::Update);
}

// Install Only (requires a previous Pull operation)
static int cli_install(LiteClient& client, const bpo::variables_map& params) {
  return install(client, params, aklite::cli::PullMode::None, aklite::cli::CheckMode::Current);
}

// Pull Only (no install performed)
static int cli_pull(LiteClient& client, const bpo::variables_map& params) {
  int version;
  std::string target_name;
  get_target_id(params, version, target_name);

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClientExt akclient{client_ptr, false, true};

  return static_cast<int>(
      aklite::cli::Pull(akclient, version, target_name, true, nullptr, aklite::cli::CheckMode::Current));
}

static int cli_complete_install(LiteClient& client, const bpo::variables_map& params) {
  // Make sure no other update instances are running, i.e. neither the daemon or other CLI update/finalize
  (void)params;
  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  // Setting apply_lock parameter to false, since we already took the FileLock above
  AkliteClient akclient{client_ptr, false, true};

  return static_cast<int>(aklite::cli::CompleteInstall(akclient));
}

// clang-format off
static const std::unordered_map<std::string, int (*)(LiteClient&, const bpo::variables_map&)> commands = {
    {"daemon", daemon_main},
    {"update", cli_update},
    {"pull", cli_pull},
    {"install", cli_install},
    {"list", list_main},
    {"check", list_main},
    {"status", status_main},
    {"finalize", cli_complete_install},
    {"run", cli_complete_install},
};
// clang-format on

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
      ("update-lockfile", bpo::value<boost::filesystem::path>(), "If provided, an flock(2) is applied to this file before performing an update in daemon mode")
      ("help,h", "print usage")
      ("version,v", "Current aktualizr version")
      ("config,c", bpo::value<std::vector<boost::filesystem::path> >()->composing(), "configuration file or directory")
      ("loglevel", bpo::value<int>(), "set log level 0-5 (trace, debug, info, warning, error, fatal)")
      ("repo-server", bpo::value<std::string>(), "URL of the Uptane repo repository")
      ("ostree-server", bpo::value<std::string>(), "URL of the Ostree repository")
      ("primary-ecu-hardware-id", bpo::value<std::string>(), "hardware ID of primary ecu")
      ("update-name", bpo::value<std::string>(), "optional name of the update when running \"update\". default=latest")
      ("install-mode", bpo::value<std::string>(), "Optional install mode. Supported modes: [delay-app-install]. By default both ostree and apps are installed before reboot")
#ifdef ALLOW_MANUAL_ROLLBACK
      ("clear-installed-versions", "DANGER - clear the history of installed updates before applying the given update. This is handy when doing test/debug and you need to rollback to an old version manually.")
#endif
      ("interval", bpo::value<uint64_t>(), "Override uptane.polling_secs interval to poll for update when in daemon mode.")
      ("command", bpo::value<std::string>(), subs.c_str());
  // clang-format on

  // consider the first positional argument as the aktualizr run mode
  bpo::positional_options_description pos;
  pos.add("command", 1);
  // consider the second positional argument as the target name / version
  pos.add("update-name", 1);

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

    config.telemetry.report_network = !config.tls.server.empty();
    config.telemetry.report_config = !config.tls.server.empty();

    LOG_DEBUG << "Current directory: " << boost::filesystem::current_path().string();

    std::string cmd = commandline_map["command"].as<std::string>();
    auto cmd_to_run = commands.find(cmd);
    if (cmd_to_run == commands.end()) {
      throw bpo::invalid_option_value("Unsupported command: " + cmd);
    }

    {
      LOG_DEBUG << "Running " << (*cmd_to_run).first;
      LiteClient client(config, nullptr);
      ret_val = (*cmd_to_run).second(client, commandline_map);
    }

  } catch (const std::exception& ex) {
    LOG_ERROR << ex.what();
    ret_val = EXIT_FAILURE;
  }

  return ret_val;
}
