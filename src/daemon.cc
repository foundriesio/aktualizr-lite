#include <sys/file.h>
#include <memory>
#include <thread>

#include "aktualizr-lite/aklite_client_ext.h"
#include "aktualizr-lite/api.h"
#include "daemon.h"
#include "libaktualizr/config.h"
#include "liteclient.h"
#include "logging/logging.h"

int run_daemon(LiteClient& client, uint64_t interval, bool return_on_sleep, bool acquire_lock) {
  if (client.config.uptane.repo_server.empty()) {
    LOG_ERROR << "[uptane]/repo_server is not configured";
    return EXIT_FAILURE;
  }
  if (access(client.config.bootloader.reboot_command.c_str(), X_OK) != 0) {
    LOG_ERROR << "reboot command: " << client.config.bootloader.reboot_command << " is not executable";
    return EXIT_FAILURE;
  }

  std::shared_ptr<LiteClient> client_ptr{&client, [](LiteClient* /*unused*/) {}};
  AkliteClientExt akclient{client_ptr, false, acquire_lock, false};
  if (akclient.IsInstallationInProgress()) {
    auto finalize_result = akclient.CompleteInstallation();
    if (finalize_result.status == InstallResult::Status::NeedsCompletion) {
      LOG_ERROR << "A system reboot is required to finalize the pending installation.";
      return EXIT_FAILURE;
    }
  }

  Uptane::HardwareIdentifier hwid(client.config.provision.primary_ecu_hardware_id);

  while (true) {
    auto current = akclient.GetCurrent();
    LOG_INFO << "Active Target: " << current.Name() << ", sha256: " << current.Sha256Hash();
    LOG_INFO << "Checking for a new Target...";
    const auto ci_res = akclient.CheckIn();
    if (ci_res) {
      auto gti_res = akclient.GetTargetToInstall(ci_res);
      if (!gti_res.selected_target.IsUnknown()) {
        LOG_INFO << "Going to install " << gti_res.selected_target.Name() << ". Reason: " << gti_res.reason;
        // A target is supposed to be installed
        auto install_result =
            akclient.PullAndInstall(gti_res.selected_target, gti_res.reason, "", InstallMode::All, nullptr, true,
                                    gti_res.status == GetTargetToInstallResult::Status::UpdateNewVersion ||
                                        gti_res.status == GetTargetToInstallResult::Status::UpdateSyncApps);
        if (akclient.RebootIfRequired()) {
          // no point to continue running TUF cycle (check for update, download, install)
          // since reboot is required to apply/finalize the currently installed update (aka pending update)
          // If a reboot command is set in configuration, and is executed successfully, we will not get to this point
          break;
        }
      }
    }

    if (return_on_sleep) {
      break;
    }

    std::this_thread::sleep_for(std::chrono::seconds(interval));
  }  // while true

  return EXIT_SUCCESS;
}
