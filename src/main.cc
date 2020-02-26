#include <iostream>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>

#include "config/config.h"
#include "helpers.h"

#include "utilities/aktualizr_version.h"

namespace bpo = boost::program_options;

static void log_info_target(const std::string &prefix, const Config &config, const Uptane::Target &t) {
  auto name = t.filename();
  if (t.custom_version().length() > 0) {
    name = t.custom_version();
  }
  LOG_INFO << prefix + name << "\tsha256:" << t.sha256Hash();
  if (config.pacman.type == PACKAGE_MANAGER_OSTREEDOCKERAPP) {
    bool shown = false;
    auto apps = t.custom_data()["docker_apps"];
    for (Json::ValueIterator i = apps.begin(); i != apps.end(); ++i) {
      if (!shown) {
        shown = true;
        LOG_INFO << "\tDocker Apps:";
      }
      if ((*i).isObject() && (*i).isMember("filename")) {
        LOG_INFO << "\t\t" << i.key().asString() << " -> " << (*i)["filename"].asString();
      } else {
        LOG_ERROR << "\t\tInvalid custom data for docker-app: " << i.key().asString();
      }
    }
  }
}

static int status_main(LiteClient &client, const bpo::variables_map &unused) {
  (void)unused;
  auto target = client.primary->getCurrent();

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

static int list_main(LiteClient &client, const bpo::variables_map &unused) {
  (void)unused;
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  LOG_INFO << "Refreshing target metadata";
  if (!client.primary->updateImagesMeta()) {
    LOG_WARNING << "Unable to update latest metadata, using local copy";
    if (!client.primary->checkImagesMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      return 1;
    }
  }

  LOG_INFO << "Updates available to " << hwid << ":";
  for (auto &t : client.primary->allTargets()) {
    if (!target_has_tags(t, client.tags)) {
      continue;
    }
    for (auto const &it : t.hardwareIds()) {
      if (it == hwid) {
        log_info_target("", client.config, t);
        break;
      }
    }
  }
  return 0;
}

static std::unique_ptr<Uptane::Target> find_target(const std::shared_ptr<SotaUptaneClient> &client,
                                                   Uptane::HardwareIdentifier &hwid,
                                                   const std::vector<std::string> &tags, const std::string &version) {
  std::unique_ptr<Uptane::Target> rv;
  if (!client->updateImagesMeta()) {
    LOG_WARNING << "Unable to update latest metadata, using local copy";
    if (!client->checkImagesMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      throw std::runtime_error("Unable to find update");
    }
  }

  bool find_latest = (version == "latest");
  std::unique_ptr<Uptane::Target> latest = nullptr;
  for (auto &t : client->allTargets()) {
    if (!target_has_tags(t, tags)) {
      continue;
    }
    for (auto const &it : t.hardwareIds()) {
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

static data::ResultCode::Numeric do_update(LiteClient &client, Uptane::Target &target) {
  target.InsertEcu(client.primary_ecu.first, client.primary_ecu.second);
  generate_correlation_id(target);

  std::unique_ptr<Lock> lock = client.getDownloadLock();
  if (lock == nullptr) {
    return data::ResultCode::Numeric::kInternalError;
  }
  client.notifyDownloadStarted(target);
  if (!client.primary->downloadImage(target).first) {
    lock->release();
    client.notifyDownloadFinished(target, false);
    return data::ResultCode::Numeric::kDownloadFailed;
  }
  lock->release();
  client.notifyDownloadFinished(target, true);

  if (client.primary->VerifyTarget(target) != TargetStatus::kGood) {
    client.notifyInstallFinished(target, data::ResultCode::Numeric::kVerificationFailed);
    LOG_ERROR << "Downloaded target is invalid";
    return data::ResultCode::Numeric::kVerificationFailed;
  }

  lock = client.getUpdateLock();
  if (lock == nullptr) {
    return data::ResultCode::Numeric::kInternalError;
  }

  client.notifyInstallStarted(target);
  auto iresult = client.primary->PackageInstall(target);
  if (iresult.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
    LOG_INFO << "Update complete. Please reboot the device to activate";
    client.storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
  } else if (iresult.result_code.num_code == data::ResultCode::Numeric::kOk) {
    LOG_INFO << "Update complete. No reboot needed";
    client.storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kCurrent);
    lock->release();
  } else {
    LOG_ERROR << "Unable to install update: " << iresult.description;
    // let go of the lock since we couldn't update
    lock->release();
  }
  client.notifyInstallFinished(target, iresult.result_code.num_code);
  return iresult.result_code.num_code;
}

static int update_main(LiteClient &client, const bpo::variables_map &variables_map) {
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  std::string version("latest");
  if (variables_map.count("update-name") > 0) {
    version = variables_map["update-name"].as<std::string>();
  }
  LOG_INFO << "Finding " << version << " to update to...";
  auto target = find_target(client.primary, hwid, client.tags, version);
  if (target == nullptr) {
    LOG_INFO << "Already up-to-date";
    return 0;
  }
  LOG_INFO << "Updating to: " << *target;
  data::ResultCode::Numeric rc = do_update(client, *target);
  if (rc == data::ResultCode::Numeric::kNeedCompletion || rc == data::ResultCode::Numeric::kOk) {
    return 0;
  }
  return 1;
}

static int daemon_main(LiteClient &client, const bpo::variables_map &variables_map) {
  if (client.config.uptane.repo_server.empty()) {
    LOG_ERROR << "[uptane]/repo_server is not configured";
    return 1;
  }
  if (access(client.config.bootloader.reboot_command.c_str(), X_OK) != 0) {
    LOG_ERROR << "reboot command: " << client.config.bootloader.reboot_command << " is not executable";
    return 1;
  }
  bool firstLoop = true;
  bool compareDockerApps = should_compare_docker_apps(client.config);
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);
  if (variables_map.count("update-lockfile") > 0) {
    client.update_lockfile = variables_map["update-lockfile"].as<boost::filesystem::path>();
  }
  if (variables_map.count("download-lockfile") > 0) {
    client.download_lockfile = variables_map["download-lockfile"].as<boost::filesystem::path>();
  }

  auto current = client.primary->getCurrent();
  LOG_INFO << "Active image is: " << current;

  uint64_t interval = client.config.uptane.polling_sec;
  if (variables_map.count("interval") > 0) {
    interval = variables_map["interval"].as<uint64_t>();
  }

  std::vector<Uptane::Target> installed_versions;
  client.storage->loadPrimaryInstallationLog(&installed_versions, false);

  while (true) {
    LOG_INFO << "Refreshing target metadata";
    if (!client.primary->updateImagesMeta()) {
      LOG_WARNING << "Unable to update latest metadata";
      std::this_thread::sleep_for(std::chrono::seconds(10));
      continue;  // There's no point trying to look for an update
    }

    if (firstLoop) {
      // On first loop we need to see if we have a config change detected from
      // from the previous run. We need to make sure we have up-to-date
      // metadata, so this really needs to be inside the loop.
      if (!current.MatchTarget(Uptane::Target::Unknown()) && client.dockerAppsChanged()) {
        do_update(client, current);
      }
      client.storeDockerParamsDigest();
      firstLoop = false;
    }

    client.primary->reportNetworkInfo();
    // reportNetworkInfo already checks `telemetry.report_network`. We need a
    // way when not running in anonymous mode to decide if we should report
    // hwinfo to the server. This flag is a good one to (ab)use.
    if (client.config.telemetry.report_network) {
      client.primary->reportHwInfo();
    }

    auto target = find_target(client.primary, hwid, client.tags, "latest");
    if (target != nullptr) {
      // do_update sets this information, so we have to set it here so that
      // targets_eq function can compare them properly
      target->InsertEcu(client.primary_ecu.first, client.primary_ecu.second);

      // This is a workaround for finding and avoiding bad updates after a rollback.
      // Rollback sets the installed version state to none instead of broken, so there is no
      // easy way to find just the bad versions without api/storage changes. As a workaround we
      // just check if the version is known (old hash) and not current/pending and abort if so
      bool known_target_sha = known_local_target(client, *target, installed_versions);
      if (known_target_sha == false && !targets_eq(*target, current, compareDockerApps)) {
        LOG_INFO << "Updating base image to: " << *target;

        data::ResultCode::Numeric rc = do_update(client, *target);
        if (rc == data::ResultCode::Numeric::kOk) {
          current = *target;
          client.http_client->updateHeader("x-ats-target", current.filename());
          // Start the loop over to call updateImagesMeta which will update this
          // device's target name on the server.
          continue;
        } else if (rc == data::ResultCode::Numeric::kNeedCompletion) {
          if (std::system(client.config.bootloader.reboot_command.c_str()) != 0) {
            LOG_ERROR << "Unable to reboot system";
            return 1;
          }
        }
      }
    }
    std::this_thread::sleep_for(std::chrono::seconds(interval));
  }
  return 0;
}

struct SubCommand {
  const char *name;
  int (*main)(LiteClient &, const bpo::variables_map &);
};
static SubCommand commands[] = {
    {"status", status_main},
    {"list", list_main},
    {"update", update_main},
    {"daemon", daemon_main},
};

void check_info_options(const bpo::options_description &description, const bpo::variables_map &vm) {
  if (vm.count("help") != 0 || vm.count("command") == 0) {
    std::cout << description << '\n';
    exit(EXIT_SUCCESS);
  }
  if (vm.count("version") != 0) {
    std::cout << "Current aktualizr version is: " << aktualizr_version() << "\n";
    exit(EXIT_SUCCESS);
  }
}

bpo::variables_map parse_options(int argc, char *argv[]) {
  std::string subs("Command to execute: ");
  for (size_t i = 0; i < sizeof(commands) / sizeof(SubCommand); i++) {
    if (i != 0) {
      subs += ", ";
    }
    subs += commands[i].name;
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
      ("repo-server", bpo::value<std::string>(), "url of the uptane repo repository")
      ("ostree-server", bpo::value<std::string>(), "url of the ostree repository")
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
  } catch (const bpo::required_option &ex) {
    // print the error and append the default commandline option description
    std::cout << ex.what() << std::endl << description;
    exit(EXIT_FAILURE);
  } catch (const bpo::error &ex) {
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

int main(int argc, char *argv[]) {
  logger_init(isatty(1) == 1);
  logger_set_threshold(boost::log::trivial::info);

  bpo::variables_map commandline_map = parse_options(argc, argv);

  int r = EXIT_FAILURE;
  try {
    if (geteuid() != 0) {
      LOG_WARNING << "\033[31mRunning as non-root and may not work as expected!\033[0m\n";
    }

    Config config(commandline_map);
    config.storage.uptane_metadata_path = BasedPath(config.storage.path / "metadata");
    config.telemetry.report_network = !config.tls.server.empty();
    LOG_DEBUG << "Current directory: " << boost::filesystem::current_path().string();

    std::string cmd = commandline_map["command"].as<std::string>();
    for (size_t i = 0; i < sizeof(commands) / sizeof(SubCommand); i++) {
      if (cmd == commands[i].name) {
        LiteClient client(config);
        return commands[i].main(client, commandline_map);
      }
    }
    throw bpo::invalid_option_value(cmd);
    r = EXIT_SUCCESS;
  } catch (const std::exception &ex) {
    LOG_ERROR << ex.what();
  }
  return r;
}
