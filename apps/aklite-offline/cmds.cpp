#include "cmds.h"
#include "composeappmanager.h"
#include "target.h"

#include "aktualizr-lite/cli/cli.h"
#include "offline/client.h"

namespace apps {
namespace aklite_offline {

int CheckCmd::checkSrcDir(const po::variables_map& vm, const boost::filesystem::path& src_dir) const {
  aklite::cli::StatusCode ret_code{EXIT_FAILURE};
  AkliteClient client(vm);
  ret_code = aklite::cli::CheckLocal(client, (src_dir / "tuf").string(), (src_dir / "ostree_repo").string(),
                                     (src_dir / "apps").string());
  return static_cast<int>(ret_code);
}

int InstallCmd::installUpdate(const Config& cfg_in, const boost::filesystem::path& src_dir,
                              bool force_downgrade) const {
  int ret_code{EXIT_FAILURE};
  try {
    const auto install_res = offline::client::install(
        cfg_in, {src_dir / "tuf", src_dir / "ostree_repo", src_dir / "apps"}, nullptr, force_downgrade);

    switch (install_res) {
      case offline::PostInstallAction::Ok: {
        std::cout << "Please run `aklite-offline run` command to start the updated Apps\n";
        ret_code = 10;
        break;
      }
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

int CurrentCmd::current(const po::variables_map& vm) const {
  int ret_code{EXIT_FAILURE};

  AkliteClient client(vm);
  auto target{client.GetCurrent()};

  boost::optional<std::vector<std::string>> cfg_apps;
  const auto cfg{client.GetConfig()};
  if (cfg.count("pacman.compose_apps") == 1) {
    std::string val = cfg.get("compose_apps", "");
    // if compose_apps is specified then `apps` optional configuration variable is initialized with an empty vector
    cfg_apps = boost::make_optional(std::vector<std::string>());
    if (val.length() > 0) {
      // token_compress_on allows lists like: "foo,bar", "foo, bar", or "foo bar"
      boost::split(*cfg_apps, val, boost::is_any_of(", "), boost::token_compress_on);
    }
  }

  std::cout << "Target: " << target.Name() << std::endl;
  std::cout << "Ostree hash: " << target.Sha256Hash() << std::endl;
  if (target.AppsJson().size() > 0) {
    std::cout << "Apps:" << std::endl;
  }
  for (const auto& app : TufTarget::Apps(target)) {
    std::string app_status =
        (!cfg_apps || std::find(cfg_apps->begin(), cfg_apps->end(), app.name) != cfg_apps->end()) ? "on " : "off";
    std::cout << "\t" << app_status << ": " << app.name + " -> " + app.uri << std::endl;
  }

  return ret_code;
}

}  // namespace aklite_offline
}  // namespace apps
