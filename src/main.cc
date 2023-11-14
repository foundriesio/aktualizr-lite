#include <iostream>
#include <thread>
#include <unordered_map>

#include <boost/algorithm/string/join.hpp>
#include <boost/container/flat_map.hpp>
#include <boost/filesystem.hpp>
#include <boost/filesystem/fstream.hpp>
#include <boost/format.hpp>
#include <boost/interprocess/sync/file_lock.hpp>
#include <boost/program_options.hpp>

#include "cli/cli.h"
#include "crypto/keymanager.h"
#include "helpers.h"
#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "storage/invstorage.h"
#include "target.h"
#include "utilities/aktualizr_version.h"

namespace bpo = boost::program_options;

static const char* const AkliteSystemLock{"/var/lock/aklite.lock"};

class FileLock {
 public:
  explicit FileLock(const char* path = AkliteSystemLock) : path_{path}, file_{path}, lock_{path} {
    if (!lock_.try_lock()) {
      throw std::runtime_error("Failed to obtain the aklite lock, another instance of aklite must be running !!!");
    }
  }

  ~FileLock() {
    try {
      lock_.unlock();
      boost::filesystem::remove(path_);
    } catch (const std::exception& exc) {
      LOG_WARNING << "Failed to unlock, " << path_ << ", error: " << exc.what();
    }
  }
  FileLock(const FileLock&) = delete;
  FileLock(const FileLock&&) = delete;
  FileLock& operator=(const FileLock&) = delete;
  FileLock& operator=(const FileLock&&) = delete;

 private:
  const char* const path_{nullptr};
  boost::filesystem::ofstream file_;
  boost::interprocess::file_lock lock_;
};

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
  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  LOG_INFO << "Refreshing Targets metadata";
  const auto rc = client.updateImageMeta();
  if (!std::get<0>(rc)) {
    LOG_WARNING << "Unable to update latest metadata, using local copy: " << std::get<1>(rc);
    if (!client.checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      return 1;
    }
  }

  boost::container::flat_map<int, Uptane::Target> sorted_targets;
  for (const auto& t : client.allTargets()) {
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
    for (const auto& it : t.hardwareIds()) {
      if (it == hwid) {
        sorted_targets.emplace(ver, t);
        break;
      }
    }
  }

  LOG_INFO << "Updates available to " << hwid << ":";
  for (auto& pair : sorted_targets) {
    client.logTarget("", pair.second);
  }
  return 0;
}

static std::pair<bool, std::unique_ptr<Uptane::Target>> find_target(LiteClient& client,
                                                                    Uptane::HardwareIdentifier& hwid,
                                                                    const std::vector<std::string>& tags,
                                                                    const std::string& version) {
  std::unique_ptr<Uptane::Target> rv;
  const auto rc = client.updateImageMeta();
  if (!std::get<0>(rc)) {
    LOG_WARNING << "Unable to update latest metadata, using local copy: " << std::get<1>(rc);
    if (!client.checkImageMetaOffline()) {
      throw std::runtime_error("Failed to use local copy of TUF data");
    }
  }

  // if a new version of targets.json hasn't been downloaded why do we do the following search
  // for the latest ??? It's really needed just for the forced update to a specific version
  bool find_latest = (version == "latest");
  std::unique_ptr<Uptane::Target> latest = nullptr;
  for (const auto& t : client.allTargets()) {
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
          if (latest == nullptr || Target::Version(latest->custom_version()) < Target::Version(t.custom_version())) {
            latest = std_::make_unique<Uptane::Target>(t);
          }
        } else if (version == t.filename() || version == t.custom_version()) {
          return {true, std_::make_unique<Uptane::Target>(t)};
        }
      }
    }
  }
  if (find_latest && latest != nullptr) {
    return {true, std::move(latest)};
  }

  return {false, std_::make_unique<Uptane::Target>(Uptane::Target::Unknown())};
}

static std::tuple<data::ResultCode::Numeric, DownloadResultWithStat, std::string> do_update(LiteClient& client,
                                                                                            Uptane::Target target,
                                                                                            const std::string& reason) {
  client.logTarget("Updating Active Target: ", client.getCurrent());
  client.logTarget("To New Target: ", target);

  generate_correlation_id(target);

  const auto download_res{client.download(target, reason)};
  if (!download_res) {
    return {data::ResultCode::Numeric::kDownloadFailed, download_res, target.correlation_id()};
  }

  if (client.VerifyTarget(target) != TargetStatus::kGood) {
    data::InstallationResult res{data::ResultCode::Numeric::kVerificationFailed, "Downloaded target is invalid"};
    client.notifyInstallFinished(target, res);
    LOG_ERROR << "Downloaded target is invalid";
    return {res.result_code.num_code, download_res, target.correlation_id()};
  }

  return {client.install(target), download_res, target.correlation_id()};
}

static data::ResultCode::Numeric do_app_sync(LiteClient& client) {
  auto target = client.getCurrent();
  LOG_INFO << "Syncing Active Target Apps";

  generate_correlation_id(target);

  const auto download_res{client.download(target, "Sync active Target Apps")};
  if (!download_res) {
    return data::ResultCode::Numeric::kDownloadFailed;
  }

  if (client.VerifyTarget(target) != TargetStatus::kGood) {
    data::InstallationResult res{data::ResultCode::Numeric::kVerificationFailed, "Downloaded target is invalid"};
    client.notifyInstallFinished(target, res);
    LOG_ERROR << "Downloaded target is invalid";
    return res.result_code.num_code;
  }

  return client.install(target);
}

static int daemon_main(LiteClient& client, const bpo::variables_map& variables_map) {
  FileLock lock;
  if (client.config.uptane.repo_server.empty()) {
    LOG_ERROR << "[uptane]/repo_server is not configured";
    return EXIT_FAILURE;
  }
  if (access(client.config.bootloader.reboot_command.c_str(), X_OK) != 0) {
    LOG_ERROR << "reboot command: " << client.config.bootloader.reboot_command << " is not executable";
    return EXIT_FAILURE;
  }

  client.importRootMetaIfNeededAndPresent();
  client.finalizeInstall();

  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);
  auto current = client.getCurrent();

  uint64_t interval = client.config.uptane.polling_sec;
  if (variables_map.count("interval") > 0) {
    interval = variables_map["interval"].as<uint64_t>();
  }

  client.reportAktualizrConfiguration();

  struct NoSpaceDownloadState {
    Hash ostree_commit_hash;
    std::string cor_id;
    storage::Volume::UsageInfo stat;
  } state_when_download_failed{Hash{"", ""}, "", {.err = "undefined"}};

  while (true) {
    LOG_INFO << "Active Target: " << current.filename() << ", sha256: " << current.sha256Hash();
    LOG_INFO << "Checking for a new Target...";

    client.reportAppsState();
    if (!client.checkForUpdatesBegin()) {
      LOG_WARNING << "Unable to update latest metadata, going to sleep for " << interval
                  << " seconds before starting a new update cycle";
      std::this_thread::sleep_for(std::chrono::seconds(interval));
      continue;  // There's no point trying to look for an update
    }

    client.reportNetworkInfo();
    client.reportHwInfo();

    std::string exc_msg;
    try {
      // try to find the latest Target for a given device
      std::pair<bool, std::unique_ptr<Uptane::Target>> find_target_res;
      try {
        find_target_res = find_target(client, hwid, client.tags, "latest");
      } catch (const std::exception& exc) {
        LOG_ERROR << "Failed to check for a new Target: " << exc.what();
        exc_msg = exc.what();
      }

      if (!find_target_res.first) {
        // TODO: consider reporting about it to the backend to make it easier to figure out
        // why specific devices are not picking up a new Target
        const auto log_msg{boost::str(boost::format("No Target found for the device; hw ID: %s; tags: %s") % hwid %
                                      boost::algorithm::join(client.tags, ","))};
        LOG_WARNING << log_msg << "; going to sleep for " << interval << " seconds before starting a new update cycle";
        // The both cases, "an exception occurs" and "no exception, but Target is not found for a device"
        // are considered as a failure for the "check-for-update-post" callback.
        client.checkForUpdatesEndWithFailure(exc_msg.empty() ? log_msg : exc_msg);
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        continue;
      }

      // Handle the case when Apps failed to start on boot just after an update
      bool rollback{false};
      Uptane::Target target_to_install{*(find_target_res.second)};
      if (client.isRollback(target_to_install) && (target_to_install.filename() == current.filename())) {
        LOG_INFO << "The currently booted Target is a failing Target, finding Target to rollback to...";
        const Uptane::Target rollback_target = client.getRollbackTarget();
        if (rollback_target.MatchTarget(Uptane::Target::Unknown())) {
          const auto log_msg{boost::str(boost::format("Failed to find Target to rollback to after a failure to start "
                                                      "Apps at boot on a new version of sysroot;"
                                                      " failing current Target: %s, hash: %s") %
                                        current.filename() % current.sha256Hash())};

          LOG_ERROR << log_msg;
          LOG_ERROR << "Going to sleep for " << interval << " seconds before starting a new update cycle";
          client.checkForUpdatesEndWithFailure(log_msg);
          std::this_thread::sleep_for(std::chrono::seconds(interval));
          continue;
        }

        LOG_INFO << "Found Target to rollback to: " << rollback_target.filename()
                 << ", hash: " << rollback_target.sha256Hash();
        target_to_install = rollback_target;
        rollback = true;
      }

      // This is a workaround for finding and avoiding bad updates after a rollback.
      // Rollback sets the installed version state to none instead of broken, so there is no
      // easy way to find just the bad versions without api/storage changes. As a workaround we
      // just check if the version is not current nor pending nor known (old hash) and never been successfully
      // installed, if so then skip an update to the such version/Target
      bool is_rollback_target = client.isRollback(target_to_install);

      if (!is_rollback_target && !client.isTargetActive(target_to_install)) {
        if (!rollback) {
          LOG_INFO << "Found new and valid Target to update to: " << target_to_install.filename()
                   << ", sha256: " << target_to_install.sha256Hash();
        }

        client.checkForUpdatesEnd(target_to_install);
        // New Target is available, try to update a device with it.
        // But prior to performing the update, check if aklite haven't tried to fetch the target ostree before,
        // and it failed due to lack of space, and the space has not increased since that time.
        if (state_when_download_failed.stat.required.first > 0 && state_when_download_failed.stat.isOk() &&
            target_to_install.MatchHash(state_when_download_failed.ostree_commit_hash)) {
          storage::Volume::UsageInfo current_usage_info{storage::Volume::getUsageInfo(
              state_when_download_failed.stat.path, state_when_download_failed.stat.reserved.second,
              state_when_download_failed.stat.reserved_by)};
          if (!current_usage_info.isOk()) {
            LOG_ERROR << "Failed to obtain storage usage statistic: " << current_usage_info.err;
          }

          if (current_usage_info.isOk() &&
              current_usage_info.available.first < state_when_download_failed.stat.required.first) {
            const std::string err_msg{
                "Insufficient storage available to download Target's ostree; hash: " + target_to_install.sha256Hash() +
                ", " + current_usage_info.withRequired(state_when_download_failed.stat.required.first).str()};
            LOG_ERROR << err_msg;
            target_to_install.setCorrelationId(state_when_download_failed.cor_id);
            client.notifyDownloadFinished(target_to_install, false, err_msg);

            std::this_thread::sleep_for(std::chrono::seconds(interval));
            continue;
          }
        }

        state_when_download_failed = {Hash{"", ""}, "", {.err = "undefined"}};
        std::string reason = std::string(rollback ? "Rolling back" : "Updating") + " from " + current.filename() +
                             " to " + target_to_install.filename();

        data::ResultCode::Numeric rc;
        DownloadResultWithStat dr;
        std::string cor_id;
        std::tie(rc, dr, cor_id) = do_update(client, target_to_install, reason);
        if (rc == data::ResultCode::Numeric::kOk) {
          current = target_to_install;
          // Start the loop over to call updateImagesMeta which will update this
          // device's target name on the server.
          continue;
        } else if (rc == data::ResultCode::Numeric::kNeedCompletion) {
          // no point to continue running TUF cycle (check for update, download, install)
          // since reboot is required to apply/finalize the currently installed update (aka pending update)
          break;
        } else if (rc == data::ResultCode::Numeric::kInstallFailed) {
          // If installation of the new Target has failed then do not wait `interval` time for the next update cycle,
          // just do it immediately in order to sync current installation with the current Target.
          // It effectively leads to either just Apps' rollback or both sysroot and Apps' rollback depending on
          // changes in the latest failing Target.
          client.setAppsNotChecked();
          continue;
        } else if (rc == data::ResultCode::Numeric::kDownloadFailed && dr.noSpace()) {
          state_when_download_failed = {Hash{Hash::Type::kSha256, target_to_install.sha256Hash()}, cor_id, dr.stat};
        }

      } else {
        if (is_rollback_target) {
          LOG_INFO << "Latest Target: " << target_to_install.filename() << " is a failing Target (aka known locally)."
                   << " Skipping its installation.";
        }
        data::ResultCode::Numeric rc{data::ResultCode::Numeric::kOk};
        if (!client.appsInSync(current)) {
          client.checkForUpdatesEnd(target_to_install);
          rc = do_app_sync(client);
          if (rc == data::ResultCode::Numeric::kOk) {
            LOG_INFO << "Device is up-to-date";
          } else {
            LOG_ERROR << "Failed to sync the current Target: " << target_to_install.filename();
          }
        } else {
          LOG_INFO << "Device is up-to-date";
          client.checkForUpdatesEnd(Uptane::Target::Unknown());
        }
      }

    } catch (const std::exception& exc) {
      LOG_ERROR << "Failed to update Target: " << exc.what();
    }

    client.setAppsNotChecked();
    std::this_thread::sleep_for(std::chrono::seconds(interval));

  }  // while true

  return EXIT_SUCCESS;
}

static int cli_install(LiteClient& client, const bpo::variables_map& params) {
  // Make sure no any other update instances are running, i.e. neither the daemon or the other CLI update/finalize is
  // running
  // The API's AkliteClient uses different type of file locking, so it cannot be used for syncing of the daemon and the
  // CLI command running. Specifically, the API's lock relies on "#define LOCK_EX 2 /* Exclusive lock.  */" while the
  // daemon's lock is based on "# define F_WRLCK 1 /* Write lock.  */"
  FileLock lock;
  std::string target_name;
  int version = -1;
  if (params.count("update-name") > 0) {
    const auto version_str = params["update-name"].as<std::string>();
    try {
      version = std::stoi(version_str);
    } catch (const std::invalid_argument& exc) {
      LOG_INFO << "Failed to convert the input target version to integer, consider it as a name of Target: "
               << exc.what();
      target_name = version_str;
    }
  }

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClient akclient{client_ptr, false, true};

  return static_cast<int>(cli::Install(akclient, version, target_name));
}

static int cli_complete_install(LiteClient& client, const bpo::variables_map& params) {
  // Make sure no any other update instances are running, i.e. neither the daemon or the other CLI update/finalize is
  // running The API's AkliteClient uses different type of file locking, so it cannot be used for syncing of the daemon
  // and the CLI command running. Specifically, the API's lock relies on "#define LOCK_EX 2 /* Exclusive lock.  */"
  // while the daemon's lock is based on "# define F_WRLCK 1 /* Write lock.  */"
  FileLock lock;
  (void)params;
  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClient akclient{client_ptr, false, true};

  return static_cast<int>(cli::CompleteInstall(akclient));
}

static const std::unordered_map<std::string, int (*)(LiteClient&, const bpo::variables_map&)> commands = {
    {"daemon", daemon_main},
    {"update", cli_install},
    {"list", list_main},
    {"status", status_main},
    {"finalize", cli_complete_install}};

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
#ifdef ALLOW_MANUAL_ROLLBACK
      ("clear-installed-versions", "DANGER - clear the history of installed updates before applying the given update. This is handy when doing test/debug and you need to rollback to an old version manually.")
#endif
      ("interval", bpo::value<uint64_t>(), "Override uptane.polling_secs interval to poll for update when in daemon mode.")
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
      LiteClient client(config, nullptr);
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
