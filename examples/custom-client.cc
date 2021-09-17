#include <thread>

#include "aktualizr-lite/api.h"
#include "logging.h"

static void reboot(const std::string &reboot_cmd) {
  LOG_INFO << "Device is going to reboot with " << reboot_cmd;
  if (setuid(0) != 0) {
    LOG_ERROR << "Failed to set/verify a root user so cannot reboot system programmatically";
  } else {
    sync();
    if (std::system(reboot_cmd.c_str()) == 0) {
      exit(0);
    }
    LOG_ERROR << "Failed to execute the reboot command";
  }
  exit(1);
}

static std::string get_reboot_cmd(const AkliteClient &client) {
  auto reboot_cmd = client.GetConfig().get("bootloader.reboot_command", "");
  // boost property_tree will include surrounding quotes which we need to
  // strip:
  if (reboot_cmd.front() == '"') {
    reboot_cmd.erase(0, 1);
  }
  if (reboot_cmd.back() == '"') {
    reboot_cmd.erase(reboot_cmd.size() - 1);
  }

  return reboot_cmd;
}

int main(int argc, char **argv) {
  AkliteClient client(AkliteClient::CONFIG_DIRS);
  auto interval = client.GetConfig().get("uptane.polling_sec", 600);
  auto reboot_cmd = get_reboot_cmd(client);

  if (access(reboot_cmd.c_str(), X_OK) != 0) {
    LOG_ERROR << "Reboot command: " << reboot_cmd << " is not executable";
    return EXIT_FAILURE;
  }

  LOG_INFO << "Starting aklite client with " << interval << " second interval";

  auto current = client.GetCurrent();
  while (true) {
    LOG_INFO << "Active Target: " << current.Name() << ", sha256: " << current.Sha256Hash();
    LOG_INFO << "Checking for a new Target...";

    try {
      auto res = client.CheckIn();
      if (res.status != CheckInResult::Status::Ok && res.status != CheckInResult::Status::OkCached) {
        LOG_WARNING << "Unable to update latest metadata, going to sleep for " << interval
                    << " seconds before starting a new update cycle";
        std::this_thread::sleep_for(std::chrono::seconds(interval));
        continue;  // There's no point trying to look for an update
      }

      auto latest = res.GetLatest();
      LOG_INFO << "Found Latest Target: " << latest.Name();
      if (latest.Name() != current.Name() && !client.IsRollback(latest)) {
        std::string reason = "Updating from " + current.Name() + " to " + latest.Name();
        auto installer = client.Installer(latest, reason);
        auto dres = installer->Download();
        if (dres.status != DownloadResult::Status::Ok) {
          LOG_ERROR << "Unable to download target: " << dres;
          continue;
        }
        auto ires = installer->Install();
        if (ires.status == InstallResult::Status::Ok) {
          current = latest;
          continue;
        } else if (ires.status == InstallResult::Status::NeedsCompletion) {
          reboot(reboot_cmd);
          break;
        } else {
          LOG_ERROR << "Unable to install target: " << ires;
        }
      } else {
        auto installer = client.CheckAppsInSync();
        if (installer != nullptr) {
          LOG_INFO << "Syncing Active Target Apps";
          auto dres = installer->Download();
          if (dres.status != DownloadResult::Status::Ok) {
            LOG_ERROR << "Unable to download target: " << dres;
          } else {
            auto ires = installer->Install();
            if (ires.status != InstallResult::Status::Ok) {
              LOG_ERROR << "Unable to install target: " << ires;
            }
          }
        }
      }
    } catch (const std::exception &exc) {
      LOG_ERROR << "Failed to find or update Target: " << exc.what();
      continue;
    }

    std::this_thread::sleep_for(std::chrono::seconds(interval));
  }
}
