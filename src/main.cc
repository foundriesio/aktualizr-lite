#include <sys/file.h>
#include <boost/log/trivial.hpp>
#include <cstddef>
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
#include <vector>

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

static int status_plain(LiteClient& client) {
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

    auto pending_target = client.getPendingTarget();
    if (pending_target.IsValid() && pending_target.filename() == target.filename()) {
      client.logTarget("Pending Target: ", target);
    } else {
      client.logTarget("Active image is: ", target);
    }
  }
  return 0;
}

static int status_json(LiteClient& client) {
  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClientExt akclient(client_ptr, true, false);
  std::cout << aklite::cli::GetStatusJson(akclient) << "\n";
  return 0;
}

static int status_main(LiteClient& client, const bpo::variables_map& params) {
  bool json_output = false;
  if (params.count("json") > 0) {
    json_output = params.at("json").as<bool>();
  }

  if (json_output) {
    return status_json(client);
  } else {
    return status_plain(client);
  }
}

static void fillUpdateSource(LocalUpdateSource& local_update_source, const std::string& src_dir) {
  const boost::filesystem::path src_dir_path(src_dir);
  local_update_source.tuf_repo = (src_dir_path / "tuf").string();
  local_update_source.ostree_repo = (src_dir_path / "ostree_repo").string();
  local_update_source.app_store = (src_dir_path / "apps").string();
}

static int checkin(LiteClient& client, aklite::cli::CheckMode check_mode, const bpo::variables_map& params) {
  bool json_output = false;
  if (params.count("json") > 0) {
    json_output = params.at("json").as<bool>();
  }

  std::string src_dir;
  if (params.count("src-dir") > 0) {
    src_dir = params.at("src-dir").as<std::string>();
  }

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClientExt akclient(client_ptr, check_mode == aklite::cli::CheckMode::Current,
                           check_mode != aklite::cli::CheckMode::Current);

  LocalUpdateSource local_update_source;
  if (!src_dir.empty()) {
    fillUpdateSource(local_update_source, src_dir);
  }
  auto status =
      aklite::cli::CheckIn(akclient, src_dir.empty() ? nullptr : &local_update_source, check_mode, json_output);
  return static_cast<int>(status);
}

static int cli_list(LiteClient& client, const bpo::variables_map& params) {
  return checkin(client, aklite::cli::CheckMode::Current, params);
}

static int cli_check(LiteClient& client, const bpo::variables_map& params) {
  return checkin(client, aklite::cli::CheckMode::Update, params);
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

  LocalUpdateSource local_update_source;
  std::string src_dir;
  if (params.count("src-dir") > 0) {
    src_dir = params.at("src-dir").as<std::string>();
  }
  if (!src_dir.empty()) {
    fillUpdateSource(local_update_source, src_dir);
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
  return static_cast<int>(aklite::cli::Install(akclient, version, target_name, mode, true,
                                               src_dir.empty() ? nullptr : &local_update_source, pull_mode,
                                               check_mode));
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

  LocalUpdateSource local_update_source;
  std::string src_dir;
  if (params.count("src-dir") > 0) {
    src_dir = params.at("src-dir").as<std::string>();
  }
  if (!src_dir.empty()) {
    fillUpdateSource(local_update_source, src_dir);
  }

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClientExt akclient{client_ptr, false, true};

  return static_cast<int>(aklite::cli::Pull(akclient, version, target_name, true,
                                            src_dir.empty() ? nullptr : &local_update_source,
                                            aklite::cli::CheckMode::Current));
}

static int cli_complete_install(LiteClient& client, const bpo::variables_map& params) {
  // Make sure no other update instances are running, i.e. neither the daemon or other CLI update/finalize
  (void)params;
  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  // Setting apply_lock parameter to false, since we already took the FileLock above
  AkliteClient akclient{client_ptr, false, true};

  return static_cast<int>(aklite::cli::CompleteInstall(akclient));
}

static int cli_rollback(LiteClient& client, const bpo::variables_map& params) {
  LocalUpdateSource local_update_source;
  std::string src_dir;
  if (params.count("src-dir") > 0) {
    src_dir = params.at("src-dir").as<std::string>();
  }
  if (!src_dir.empty()) {
    fillUpdateSource(local_update_source, src_dir);
  }

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  // Setting apply_lock parameter to false, since we already took the FileLock above
  AkliteClientExt akclient{client_ptr, false, true};

  return static_cast<int>(aklite::cli::Rollback(akclient, src_dir.empty() ? nullptr : &local_update_source));
}

struct Cmd {
  const std::string name;
  const std::string description;
  int (*func)(LiteClient&, const bpo::variables_map&);
  bool read_only_storage{false};
};

// clang-format off
static const std::array<Cmd, 10> commands = {{
    {"daemon", "Start the update agent daemon", daemon_main},
    {"update", "Update TUF metadata, download and install the selected target", cli_update},
    {"pull", "Download the selected target data to the device, to allow a install operation to be performed", cli_pull},
    {"install", "Install a previously pulled target", cli_install},
    {"list", "List the available targets, using current TUF metadata information. No TUF update is performed", cli_list, true},
    {"check", "Update the device TUF metadata, and list the available targets", cli_check},
    {"status", "Show information of the target currently running on the device", status_main, true},
    {"finalize", "Finalize installation by starting the updated apps", cli_complete_install},
    {"run", "Alias for the finalize command", cli_complete_install},
    {"rollback", "Rollback to the previous successfully installed target [experimental]", cli_rollback},
}};
// clang-format on
std::string get_cmds_list() {
  std::string ret;
  ret += "Commands:\n";
  for (const auto& cmd : commands) {
    auto name = cmd.name;
    ret += "  " + name.append(12 - name.length(), ' ') + cmd.description + "\n";
  }
  return ret;
}

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

int run_command(const Cmd& cmd, const bpo::variables_map& commandline_map) {
  Config config(commandline_map);

  if (geteuid() != 0) {
    LOG_WARNING << "\033[31mRunning as non-root and may not work as expected!\033[0m\n";
  }

  LOG_DEBUG << "Current directory: " << boost::filesystem::current_path().string();
  config.telemetry.report_network = !config.tls.server.empty();
  config.telemetry.report_config = !config.tls.server.empty();
  LOG_DEBUG << "Running " << cmd.name;
  LiteClient client(config, nullptr, nullptr, nullptr, cmd.read_only_storage);
  return cmd.func(client, commandline_map);
}

int run_command_by_name(const std::string& cmd_name, const bpo::variables_map& commandline_map) {
  for (const auto& cmd : commands) {
    if (cmd.name == cmd_name) {
      return run_command(cmd, commandline_map);
    }
  }
  throw bpo::invalid_option_value("Unsupported command: " + cmd_name);
}

bpo::variables_map parse_options(int argc, char** argv) {
  bpo::options_description description("Usage:\n  aktualizr-lite [command] [flags]\n\n" + get_cmds_list() + "\nFlags");

  // clang-format off
  // Try to keep these options in the same order as Config::updateFromCommandLine().
  // The first three are commandline only.
  description.add_options()
      ("help,h", "Print usage")
      ("version,v", "Prints current aktualizr-lite version")
      ("config,c", bpo::value<std::vector<boost::filesystem::path> >()->composing(), "Configuration file or directory path")
      ("loglevel", bpo::value<int>(), "Set log level 0-5 (trace, debug, info, warning, error, fatal)")
      ("update-name", bpo::value<std::string>(), "Name or version of the target to be used in pull, install, and update commands. default=latest")
      ("install-mode", bpo::value<std::string>(), "Optional install mode. Supported modes: [delay-app-install]. By default both ostree and apps are installed before reboot")
#ifdef ALLOW_MANUAL_ROLLBACK
      ("clear-installed-versions", "DANGER - clear the history of installed updates before applying the given update. This is handy when doing test/debug and you need to rollback to an old version manually")
#endif
      ("interval", bpo::value<uint64_t>(), "Override uptane.polling_secs interval to poll for updates when in daemon mode")
      ("json", bpo::value<bool>(), "Output targets information as json when running status, check, and list commands")
      ("src-dir", bpo::value<std::string>(), "Directory that contains an offline update bundle. Enables offline mode for check, pull, install, and update commands")
      ("command", bpo::value<std::string>(), "Command to be executed");
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
    // When enabling json output mode, hide log messages by default
    if (commandline_map.count("json") > 0 && commandline_map.at("json").as<bool>() &&
        commandline_map.count("loglevel") == 0) {
      commandline_map.insert(std::pair<std::string, boost::program_options::variable_value>(
          "loglevel", boost::program_options::variable_value(static_cast<int>(boost::log::trivial::fatal), false)));
    }

    std::string cmd_name = commandline_map["command"].as<std::string>();
    ret_val = run_command_by_name(cmd_name, commandline_map);
  } catch (const std::exception& ex) {
    LOG_ERROR << ex.what();
    ret_val = EXIT_FAILURE;
  }

  return ret_val;
}
