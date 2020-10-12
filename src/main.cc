#include <iostream>
#include <unordered_map>

#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "helpers.h"
#include "libaktualizr/config.h"

#include "utilities/aktualizr_version.h"

namespace bpo = boost::program_options;

static int status_main(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;
  auto target = client.getCurrent();

  if (target.MatchTarget(Uptane::Target::Unknown())) {
    LOG_INFO << "No active deployment found";
  } else {
    auto name = target.filename();
    if (target.custom_version().length() > 0) {
      name = target.custom_version();
    }
    log_info_target("Active image is: ", client.config, target);
  }
  return 0;
}

static int list_main(LiteClient& client, const bpo::variables_map& unused) {
  (void)unused;
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  LOG_INFO << "Refreshing Targets metadata";
  if (!client.updateImageMeta()) {
    LOG_WARNING << "Unable to update latest metadata, using local copy";
    if (!client.checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      return 1;
    }
  }

  boost::container::flat_map<int, Uptane::Target> sorted_targets;
  for (auto& t : client.allTargets()) {
    int ver = 0;
    try {
      ver = std::stoi(t.custom_version(), nullptr, 0);
    } catch (const std::invalid_argument& exc) {
      LOG_ERROR << "Invalid version number format: " << t.custom_version();
      ver = -1;
    }
    if (!target_has_tags(t, client.tags)) {
      continue;
    }
    for (auto const& it : t.hardwareIds()) {
      if (it == hwid) {
        sorted_targets.emplace(ver, t);
        break;
      }
    }
  }

  LOG_INFO << "Updates available to " << hwid << ":";
  for (auto& pair : sorted_targets) {
    log_info_target("", client.config, pair.second);
  }
  return 0;
}

static std::unique_ptr<Uptane::Target> find_target(LiteClient& client, Uptane::HardwareIdentifier& hwid,
                                                   const std::vector<std::string>& tags, const std::string& version) {
  std::unique_ptr<Uptane::Target> rv;
  if (!client.updateImageMeta()) {
    LOG_WARNING << "Unable to update latest metadata, using local copy";
    if (!client.checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      throw std::runtime_error("Unable to find update");
    }
  }

  bool find_latest = (version == "latest");
  std::unique_ptr<Uptane::Target> latest = nullptr;
  for (auto& t : client.allTargets()) {
    if (!t.IsValid()) {
      continue;
    }

    if (!t.IsOstree()) {
      continue;
    }

    if (!target_has_tags(t, tags)) {
      continue;
    }
    for (auto const& it : t.hardwareIds()) {
      if (it == hwid) {
        if (find_latest) {
          if (latest == nullptr || Version(latest->custom_version()) < Version(t.custom_version())) {
            latest = std_::make_unique<Uptane::Target>(t);
          }
        } else if (version == t.filename() || version == t.custom_version()) {
          return std_::make_unique<Uptane::Target>(t);
        }
      }
    }
  }
  if (find_latest && latest != nullptr) {
    return latest;
  }
  throw std::runtime_error("Unable to find update");
}

static data::ResultCode::Numeric do_update(LiteClient& client, Uptane::Target target) {
  log_info_target("Updating Active Target: ", client.config, client.getCurrent());
  log_info_target("To New Target: ", client.config, target);

  generate_correlation_id(target);

  data::ResultCode::Numeric rc = client.download(target);
  if (rc != data::ResultCode::Numeric::kOk) {
    return rc;
  }

  if (client.VerifyTarget(target) != TargetStatus::kGood) {
    client.notifyInstallFinished(target, data::ResultCode::Numeric::kVerificationFailed);
    LOG_ERROR << "Downloaded target is invalid";
    return data::ResultCode::Numeric::kVerificationFailed;
  }

  return client.install(target);
}

static int update_main(LiteClient& client, const bpo::variables_map& variables_map) {
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  std::string version("latest");

  if (variables_map.count("update-name") > 0) {
    version = variables_map["update-name"].as<std::string>();
  }

  LOG_INFO << "Finding " << version << " to update to...";
  auto target_to_install = find_target(client, hwid, client.tags, version);

  data::ResultCode::Numeric rc = do_update(client, *target_to_install);

  return (rc == data::ResultCode::Numeric::kNeedCompletion || rc == data::ResultCode::Numeric::kOk) ? EXIT_SUCCESS
                                                                                                    : EXIT_FAILURE;
}

static int daemon_main(LiteClient& client, const bpo::variables_map& variables_map) {
  if (client.config.uptane.repo_server.empty()) {
    LOG_ERROR << "[uptane]/repo_server is not configured";
    return EXIT_FAILURE;
  }
  if (access(client.config.bootloader.reboot_command.c_str(), X_OK) != 0) {
    LOG_ERROR << "reboot command: " << client.config.bootloader.reboot_command << " is not executable";
    return EXIT_FAILURE;
  }
  bool firstLoop = true;
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);
  if (variables_map.count("update-lockfile") > 0) {
    client.update_lockfile = variables_map["update-lockfile"].as<boost::filesystem::path>();
  }
  if (variables_map.count("download-lockfile") > 0) {
    client.download_lockfile = variables_map["download-lockfile"].as<boost::filesystem::path>();
  }

  auto current = client.getCurrent();

  uint64_t interval = client.config.uptane.polling_sec;
  if (variables_map.count("interval") > 0) {
    interval = variables_map["interval"].as<uint64_t>();
  }

  std::vector<Uptane::Target> installed_versions;
  client.storage->loadPrimaryInstallationLog(&installed_versions, false);

  client.reportAktualizrConfiguration();

  while (true) {
    LOG_INFO << "Active Target: " << current.filename() << ", sha256: " << current.sha256Hash();
    LOG_INFO << "Checking for a new Target...";
    if (!client.checkForUpdates()) {
      LOG_WARNING << "Unable to update latest metadata";
      std::this_thread::sleep_for(std::chrono::seconds(10));
      continue;  // There's no point trying to look for an update
    }

    if (firstLoop) {
      // On first loop we need to see if we have a config change detected from
      // from the previous run. We need to make sure we have up-to-date
      // metadata, so this really needs to be inside the loop.
      // Also, check if Apps are actually installed and running
      if (current.IsValid() && client.checkAppsToUpdate(current)) {
        do_update(client, current);
      }
      firstLoop = false;
    }

    client.reportNetworkInfo();
    client.reportHwInfo();

    try {
      // target cannot be nullptr, an exception will be yielded if no target
      auto target = find_target(client, hwid, client.tags, "latest");

      // This is a workaround for finding and avoiding bad updates after a rollback.
      // Rollback sets the installed version state to none instead of broken, so there is no
      // easy way to find just the bad versions without api/storage changes. As a workaround we
      // just check if the version is known (old hash) and not current/pending and abort if so
      bool known_target_sha = known_local_target(client, *target, installed_versions);
      if (!known_target_sha && !client.isTargetCurrent(*target)) {
        LOG_INFO << "Got a New Target!";

        data::ResultCode::Numeric rc = do_update(client, *target);
        if (rc == data::ResultCode::Numeric::kOk) {
          current = *target;
          client.http_client->updateHeader("x-ats-target", current.filename());
          // Start the loop over to call updateImagesMeta which will update this
          // device's target name on the server.
          continue;
        } else if (rc == data::ResultCode::Numeric::kNeedCompletion) {
          // no point to continue running TUF cycle (check for update, download, install)
          // since reboot is required to apply/finalize the currently installed update (aka pending update)
          break;
        }

      } else {
        LOG_INFO << "Device is up-to-date";
      }

    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to find or update Target: " << exc.what();
    }

    client.setAppsNotChecked();
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
  if (vm.count("help") != 0 || vm.count("command") == 0) {
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
      LiteClient client(config);
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
