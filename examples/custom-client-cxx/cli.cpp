#include <iostream>
#include <thread>

#include <boost/filesystem.hpp>
#include <boost/log/core.hpp>
#include <boost/log/expressions.hpp>
#include <boost/log/trivial.hpp>
#include <boost/process.hpp>

#include "aktualizr-lite/api.h"
#include "aktualizr-lite/cli/cli.h"
#include "aktualizr-lite/aklite_client_ext.h"
#include "cli.h"

#define LOG_INFO BOOST_LOG_TRIVIAL(info)
#define LOG_WARNING BOOST_LOG_TRIVIAL(warning)
#define LOG_ERROR BOOST_LOG_TRIVIAL(error)

static void print_status(aklite::cli::StatusCode ret) {
  switch (ret) {
    case aklite::cli::StatusCode::Ok:
      std::cout << "SUCCESS";
      break;

    // Possible return codes for check, pull and install commands
    case aklite::cli::StatusCode::CheckinOkCached:
      std::cout << "SUCCESS: Unable to fetch updated TUF metadata, but stored metadata is valid";
      break;
    case aklite::cli::StatusCode::CheckinFailure:
      std::cout << "FAILURE: Failed to update TUF metadata";
      break;
    case aklite::cli::StatusCode::CheckinNoMatchingTargets:
      std::cout << "FAILURE: There is no matching target for the device";
      break;
    case aklite::cli::StatusCode::CheckinNoTargetContent:
      std::cout << "FAILURE: There is no target metadata in the local path";
      break;
    case aklite::cli::StatusCode::CheckinSecurityError:
      std::cout << "FAILURE: Invalid TUF metadata";
      break;
    case aklite::cli::StatusCode::CheckinExpiredMetadata:
      std::cout << "FAILURE: TUF metadata is expired";
      break;
    case aklite::cli::StatusCode::CheckinMetadataFetchFailure:
      std::cout << "FAILURE: Unable to fetch TUF metadata";
      break;
    case aklite::cli::StatusCode::TufTargetNotFound:
      std::cout << "FAILURE: Selected target not found";
      break;

    // Possible return codes for pull and install commands
    case aklite::cli::StatusCode::InstallationInProgress:
      std::cout << "FAILURE: Unable to pull/install: there is an installation that needs completion";
      break;
    case aklite::cli::StatusCode::DownloadFailure:
      std::cout << "FAILURE: Unable to download target";
      break;
    case aklite::cli::StatusCode::DownloadFailureVerificationFailed:
      std::cout << "FAILURE: Target downloaded but verification has failed";
      break;
    case aklite::cli::StatusCode::DownloadFailureNoSpace:
      std::cout << "FAILURE: There is no enough free space to download the target";
      break;
    case aklite::cli::StatusCode::InstallAlreadyInstalled:
      std::cout << "FAILURE: Selected target is already installed";
      break;
    case aklite::cli::StatusCode::InstallDowngradeAttempt:
      // Should not hit this error, since force_downgrade is set to true
      std::cout << "FAILURE: Attempted to install a previous version";
      break;

    // Possible return codes for install command
    case aklite::cli::StatusCode::InstallAppsNeedFinalization:
      std::cout << "SUCCESS: Execute `custom-sota-client run` command to finalize installation";
      break;
    case aklite::cli::StatusCode::InstallNeedsRebootForBootFw:
      std::cout << "FAILURE: Reboot is required before installing the target";
      break;
    case aklite::cli::StatusCode::InstallAppPullFailure:
      std::cout << "FAILURE: Unable read target data, make sure it was pulled";
      break;

    // Possible return codes for install and run command
    case aklite::cli::StatusCode::InstallNeedsReboot:
      std::cout << "SUCCESS: Reboot to finalize installation";
      break;
    case aklite::cli::StatusCode::OkNeedsRebootForBootFw:
      std::cout << "SUCCESS: Reboot to finalize bootloader installation";
      break;
    case aklite::cli::StatusCode::InstallRollbackNeedsReboot:
      std::cout << "FAILURE: Installation failed, rollback initiated but requires reboot to finalize";
      break;

    // Possible return codes for run command
    case aklite::cli::StatusCode::NoPendingInstallation:
      std::cout << "FAILURE: No pending installation to run";
      break;
    case aklite::cli::StatusCode::InstallRollbackOk:
    case aklite::cli::StatusCode::InstallOfflineRollbackOk:
      std::cout << "FAILURE: Installation failed, rollback performed";
      break;
    case aklite::cli::StatusCode::InstallRollbackFailed:
      std::cout << "FAILURE: Installation failed and rollback operation was not successful";
      break;
    case aklite::cli::StatusCode::UnknownError:
      std::cout << "FAILURE: Unknown error";
      break;

    default:
      std::cout << "FAILURE: Unexpected return code " << static_cast<int>(ret);
      break;
  }
  std::cout << std::endl;
}

static std::unique_ptr<AkliteClientExt> init_client(bool online_mode) {
  boost::log::core::get()->set_filter(boost::log::trivial::severity >= boost::log::trivial::info);

  std::vector<boost::filesystem::path> cfg_dirs;

  auto env{boost::this_process::environment()};
  if (env.end() != env.find("AKLITE_CONFIG_DIR")) {
    cfg_dirs.emplace_back(env.get("AKLITE_CONFIG_DIR"));
  } else if (online_mode) {
    cfg_dirs = AkliteClient::CONFIG_DIRS;
  } else {
    // sota.toml is optional in offline mode
    for (const auto& cfg : AkliteClient::CONFIG_DIRS) {
      if (boost::filesystem::exists(cfg)) {
        cfg_dirs.emplace_back(cfg);
      }
    }
  }

  try {
    return std::make_unique<AkliteClientExt>(cfg_dirs, false, false);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to initialize the client: " << exc.what();
    return nullptr;
  }
}

int cmd_check(std::string local_repo_path) {
  auto client = init_client(local_repo_path.empty());
  if (client == nullptr) {
    return EXIT_FAILURE;
  }

  LocalUpdateSource local_update_source;
  if (local_repo_path.empty()) {
    LOG_INFO << "Online mode";
  } else {
    auto abs_repo_path = boost::filesystem::canonical(local_repo_path).string();
    LOG_INFO << "Offline mode. Updates path=" << abs_repo_path;
    local_update_source = {abs_repo_path + "/tuf", abs_repo_path + "/ostree_repo", abs_repo_path + "/apps"};
  }

  auto status = aklite::cli::CheckIn(*client, local_repo_path.empty() ? nullptr : &local_update_source);
  print_status(status);
  return aklite::cli::IsSuccessCode(status) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_pull(std::string target_name, std::string local_repo_path) {
  auto client = init_client(local_repo_path.empty());
  if (client == nullptr) {
    return EXIT_FAILURE;
  }

  LocalUpdateSource local_update_source;
  if (local_repo_path.empty()) {
    LOG_INFO << "Online mode";
  } else {
    auto abs_repo_path = boost::filesystem::canonical(local_repo_path).string();
    LOG_INFO << "Offline mode. Updates path=" << abs_repo_path;
    local_update_source = {abs_repo_path + "/tuf", abs_repo_path + "/ostree_repo", abs_repo_path + "/apps"};
  }

  auto status =
      aklite::cli::Pull(*client, -1, target_name, true, local_repo_path.empty() ? nullptr : &local_update_source);
  print_status(status);
  return aklite::cli::IsSuccessCode(status) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_install(std::string target_name, std::string local_repo_path) {
  auto client = init_client(local_repo_path.empty());
  if (client == nullptr) {
    return EXIT_FAILURE;
  }

  LocalUpdateSource local_update_source;
  if (local_repo_path.empty()) {
    LOG_INFO << "Online mode";
  } else {
    auto abs_repo_path = boost::filesystem::canonical(local_repo_path).string();
    LOG_INFO << "Offline mode. Updates path=" << abs_repo_path;
    local_update_source = {abs_repo_path + "/tuf", abs_repo_path + "/ostree_repo", abs_repo_path + "/apps"};
  }

  auto status =
      aklite::cli::Install(*client, -1, target_name, InstallMode::All, true,
                           local_repo_path.empty() ? nullptr : &local_update_source, aklite::cli::PullMode::None);
  print_status(status);
  return aklite::cli::IsSuccessCode(status) ? EXIT_SUCCESS : EXIT_FAILURE;
}

int cmd_run() {
  auto client = init_client(false);
  if (client == nullptr) {
    return EXIT_FAILURE;
  }

  auto status = aklite::cli::CompleteInstall(*client);
  print_status(status);
  return aklite::cli::IsSuccessCode(status) ? EXIT_SUCCESS : EXIT_FAILURE;
}
