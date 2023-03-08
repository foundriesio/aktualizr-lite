#include "cmds.h"

#include "offline/client.h"

namespace apps {
namespace aklite_offline {

int InstallCmd::installUpdate(const Config& cfg_in, const boost::filesystem::path& src_dir) const {
  int ret_code{EXIT_FAILURE};
  try {
    const auto install_res =
        offline::client::install(cfg_in, {src_dir / "tuf", src_dir / "ostree_repo", src_dir / "apps"});

    switch (install_res) {
      case offline::PostInstallAction::NeedReboot: {
        ret_code = 100;
        std::cout << "Please reboot a device and run `run` command to apply installation and start the updated Apps "
                     "(unless no Apps to update or dockerless system)\n";
        break;
      }
      case offline::PostInstallAction::NeedRebootToUpdateBootFw: {
        ret_code = 110;
        std::cout << "Please reboot a device to finalize the boot firmware update and then re-run the `install` "
                     "command again";
        break;
      }
      case offline::PostInstallAction::NeedDockerRestart: {
        std::cout << "Please restart `docker` service `systemctl restart docker` and run `run` command to start the "
                     "updated Apps\n";
        ret_code = 101;
        break;
      }
      case offline::PostInstallAction::AlreadyInstalled: {
        std::cout << "The given Target has been already installed\n";
        ret_code = EXIT_SUCCESS;
        break;
      }
      default:
        LOG_ERROR << "Got invalid post install action code";
    };

  } catch (const std::exception& exc) {
    std::cerr << "Failed to install the offline update; src-dir: " << src_dir << "; err: " << exc.what();
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
      case offline::PostRunAction::RollbackNeedReboot: {
        LOG_INFO << "Installation or Apps start has failed so a device was rolled backed to a previous version";
        LOG_INFO << "Reboot is required to complete the rollback";
        ret_code = 100;
        break;
      }
      case offline::PostRunAction::NoUpdateToRun: {
        LOG_INFO << "No new Target to run has been found";
        ret_code = 110;
        break;
      }
      case offline::PostRunAction::NeedReboot: {
        LOG_INFO << "Please, reboot a device before finalizing the update and running the updated Apps";
        ret_code = 120;
        break;
      }
      case offline::PostRunAction::Failed: {
        LOG_INFO
            << "Failed to finalize the update; no rollback is needed since a device is running the previous version";
        ret_code = 130;
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

}  // namespace aklite_offline
}  // namespace apps
