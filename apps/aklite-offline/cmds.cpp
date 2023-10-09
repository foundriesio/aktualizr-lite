#include "cmds.h"
#include "composeappmanager.h"
#include "target.h"

#include "offline/client.h"

namespace apps {
namespace aklite_offline {

int CheckCmd::checkSrcDir(const Config& cfg_in, const boost::filesystem::path& src_dir) const {
  int ret_code{EXIT_FAILURE};
  try {
    const auto found_targets =
        offline::client::check(cfg_in, {src_dir / "tuf", src_dir / "ostree_repo", src_dir / "apps"});

    std::cout << "\nFound Targets: " << std::endl;
    for (const auto& t : found_targets) {
      std::cout << "\tName: " << t.filename() << std::endl;
      std::cout << "\tOSTree hash: " << t.sha256Hash() << std::endl;
      std::cout << "\tApps:" << std::endl;
      for (const auto& a : Target::Apps(t)) {
        std::cout << "\t\t" << a.name << " -> " << a.uri << std::endl;
      }
      std::cout << std::endl;
    }
  } catch (const std::exception& exc) {
    std::cerr << "Failed to check the update source directory; src-dir: " << src_dir << "; err: " << exc.what();
  }
  return ret_code;
}

int InstallCmd::installUpdate(const Config& cfg_in, const boost::filesystem::path& src_dir,
                              bool force_downgrade) const {
  int ret_code{EXIT_FAILURE};
  try {
    const auto install_res = offline::client::install(
        cfg_in, {src_dir / "tuf", src_dir / "ostree_repo", src_dir / "apps"}, nullptr, force_downgrade);

    switch (install_res) {
      case offline::PostInstallAction::NeedRebootForBootFw: {
        ret_code = 90;
        std::cout
            << "Please reboot a device to confirm a boot firmware update, and then run the `install` command again\n";
        std::cout << "If the reboot doesn't help to proceed with the update, then make sure that "
                     "`bootupgrade_available` is set to `0`.";
        std::cout << "Try running `fw_setenv|fiovb_setenv bootupgrade_available 0`, reboot a device, and then run "
                     "the `install` again";
        break;
      }
      case offline::PostInstallAction::NeedReboot: {
        ret_code = 100;
        std::cout
            << "Please reboot a device and execute `aklite-offline run` command "
               "to apply installation and start the updated Apps (unless no Apps to update or dockerless system)\n";
        break;
      }
      case offline::PostInstallAction::NeedDockerRestart: {
        std::cout << "Please restart `docker` service `systemctl restart docker` and execute `aklite-offline run` "
                     "command to start the updated Apps\n";
        ret_code = 101;
        break;
      }
      case offline::PostInstallAction::AlreadyInstalled: {
        std::cout << "The given Target has been already installed\n";
        ret_code = EXIT_SUCCESS;
        break;
      }
      case offline::PostInstallAction::DowngradeAttempt: {
        std::cout << "Refused to downgrade\n";
        ret_code = 102;
        break;
      }
      default:
        LOG_ERROR << "Got invalid post install action code";
    };

  } catch (const std::exception& exc) {
    std::cerr << "Failed to install the offline update; src-dir: " << src_dir << "; err: " << exc.what() << std::endl;
  }
  return ret_code;
}

int RunCmd::runUpdate(const Config& cfg_in) const {
  int ret_code{EXIT_FAILURE};

  try {
    const auto run_res = offline::client::run(cfg_in);
    switch (run_res) {
      case offline::PostRunAction::Ok: {
        LOG_INFO << "Successfully applied new version of rootfs and started Apps if present";
        ret_code = 0;
        break;
      }
      case offline::PostRunAction::OkNoPendingInstall: {
        LOG_INFO << "No pending installation to run/finalize has been found."
                 << " Make sure you called `install` before `run`.";
        ret_code = 0;
        break;
      }
      case offline::PostRunAction::OkNeedReboot: {
        LOG_INFO << "Successfully applied new version of rootfs and started Apps if present.";
        LOG_INFO << "Please, optionally reboot a device to confirm the boot firmware update;"
                    " the reboot can be performed now, anytime later, or at the beginning of the next update";
        ret_code = 90;
        break;
      }
      case offline::PostRunAction::RollbackOk: {
        LOG_INFO << "Installation has failed so a device rollbacked to the previous version";
        LOG_INFO << "No reboot is required";
        ret_code = 99;
        break;
      }
      case offline::PostRunAction::RollbackNeedReboot: {
        LOG_INFO << "Apps start has failed so a device is rollbacking to the previous version";
        LOG_INFO << "Please reboot a device and execute `aklite-offline run` command to complete the rollback";
        ret_code = 100;
        break;
      }
      case offline::PostRunAction::RollbackToUnknown: {
        LOG_INFO << "Installation has failed so a device rollbacked to the previous version";
        LOG_INFO << "No reboot is required; Apps are in undefined state";
        ret_code = 110;
        break;
      }
      case offline::PostRunAction::RollbackToUnknownIfAppFailed: {
        LOG_INFO << "Apps start has failed after successful boot on the updated rootfs";
        LOG_INFO << "Apps are in undefined state";
        ret_code = 120;
        break;
      }
      case offline::PostRunAction::RollbackFailed: {
        LOG_INFO << "Apps start has failed while rollbacking to the previous version";
        LOG_INFO << "Apps are in undefined state";
        ret_code = 120;
        break;
      }
      default:
        LOG_ERROR << "Got invalid post run action code";
    };
  } catch (const std::exception& exc) {
    std::cerr << "Failed to run the offline update; err: " << exc.what();
  }
  return ret_code;
}

int CurrentCmd::current(const Config& cfg_in) const {
  int ret_code{EXIT_FAILURE};

  auto target = offline::client::getCurrent(cfg_in, nullptr);

  ComposeAppManager::Config pc{cfg_in.pacman};

  std::cout << "Target: " << target.filename() << std::endl;
  std::cout << "Ostree hash: " << target.sha256Hash() << std::endl;
  std::cout << "Apps:" << std::endl;
  for (const auto& app : Target::Apps(target)) {
    if (!pc.apps || (*pc.apps).end() != std::find((*pc.apps).begin(), (*pc.apps).end(), app.name)) {
      std::cout << "    " << app.name + " -> " + app.uri << std::endl;
    }
  }

  return ret_code;
}

}  // namespace aklite_offline
}  // namespace apps
