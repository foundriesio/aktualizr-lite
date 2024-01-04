#include "cmds.h"
#include "composeappmanager.h"
#include "target.h"

#include "aktualizr-lite/cli/cli.h"
#include "offline/client.h"

namespace apps {
namespace aklite_offline {

int CheckCmd::checkSrcDir(const po::variables_map& vm, const boost::filesystem::path& src_dir) const {
  AkliteClient client(vm);
  const auto ret_code{aklite::cli::CheckLocal(client, (src_dir / "tuf").string(), (src_dir / "ostree_repo").string(),
                                              (src_dir / "apps").string())};
  return static_cast<int>(ret_code);
}

int InstallCmd::installUpdate(const po::variables_map& vm, const boost::filesystem::path& src_dir,
                              bool force_downgrade) const {
  AkliteClient client(vm);
  const LocalUpdateSource local_update_source{.tuf_repo = (src_dir / "tuf").string(),
                                              .ostree_repo = (src_dir / "ostree_repo").string(),
                                              .app_store = (src_dir / "apps").string()};
  auto ret_code{aklite::cli::Install(client, -1, "", "", force_downgrade, &local_update_source)};
  switch (ret_code) {
    case aklite::cli::StatusCode::InstallAppsNeedFinalization: {
      // TBD: The former `aklite-offline` sets `10` as a an exit/status code, while
      // the current version returns `InstallAppsNeedFinalization = 105`.
      // Maybe it makes sense to override it with `10`, but the `10` is already used for `TufMetaPullFailure = 10`?
      std::cout << "Please run `aklite-offline run` command to start the updated Apps\n";
      break;
    }
    case aklite::cli::StatusCode::InstallNeedsRebootForBootFw: {
      std::cout
          << "Please reboot a device to confirm a boot firmware update, and then run the `install` command again\n";
      std::cout << "If the reboot doesn't help to proceed with the update, then make sure that "
                   "`bootupgrade_available` is set to `0`.";
      std::cout << "Try running `fw_setenv|fiovb_setenv bootupgrade_available 0`, reboot a device, and then run "
                   "the `install` again";
      break;
    }
    case aklite::cli::StatusCode::InstallNeedsReboot: {
      std::cout << "Please reboot a device and run `aklite-offline run` command "
                   "to apply installation and start the updated Apps (unless no Apps to update or dockerless system)\n";
      break;
    }
    case aklite::cli::StatusCode::InstallAlreadyInstalled: {
      std::cout << "The given Target has been already installed\n";
      ret_code = aklite::cli::StatusCode::Ok;
      break;
    }
    case aklite::cli::StatusCode::InstallDowngradeAttempt: {
      std::cout << "Refused to downgrade\n";
      break;
    }
    default:
      break;
  }
  return static_cast<int>(ret_code);
}

int RunCmd::runUpdate(const po::variables_map& vm) const {
  AkliteClient client(vm, false, false);
  const auto ret_code{aklite::cli::CompleteInstall(client)};
  return static_cast<int>(ret_code);
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
