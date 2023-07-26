#include "rootfstreemanager.h"

#include <boost/algorithm/string.hpp>

#include "ostree/repo.h"
#include "storage/invstorage.h"
#include "target.h"

RootfsTreeManager::RootfsTreeManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                                     const std::shared_ptr<INvStorage>& storage,
                                     const std::shared_ptr<HttpInterface>& http,
                                     std::shared_ptr<OSTree::Sysroot> sysroot, const KeyManager& keys)
    : OstreeManager(pconfig, bconfig, storage, http, new bootloader::BootloaderLite(bconfig, *storage, sysroot)),
      sysroot_{std::move(sysroot)},
      boot_fw_update_status_{new bootloader::BootloaderLite(bconfig, *storage, sysroot_)},
      http_client_{http},
      gateway_url_{pconfig.ostree_server},
      keys_{keys} {
  const std::string update_block_attr_name{"ostree_update_block"};
  if (pconfig.extra.count(update_block_attr_name) == 1) {
    std::string val{pconfig.extra.at(update_block_attr_name)};
    update_block_ = val != "0" && val != "false";
  }
}

DownloadResult RootfsTreeManager::Download(const TufTarget& target) {
  auto prog_cb = [this](const Uptane::Target& t, const std::string& description, unsigned int progress) {
    // report_progress_cb(events_channel.get(), t, description, progress);
    // TODO: consider make use of it for download progress reporting
  };

  std::vector<Remote> remotes = {{remote, config.ostree_server, {{"X-Correlation-ID", target.Name()}}, &keys_, false}};

  // Try to get additional remotes/origins to fetch an ostree commit from, unless
  // the base ostree server URL specified in a config refers not to http(s) server.
  // It helps to skip getting additional remotes if `ostree_server` refers to a local
  // ostree repo, i.e. file://<path to repo>
  if (!config.ostree_server.empty() && boost::starts_with(config.ostree_server, "http")) {
    getAdditionalRemotes(remotes, target.Name());
  }

  DownloadResult res{DownloadResult::Status::Ok, ""};
  data::InstallationResult pull_err{data::ResultCode::Numeric::kUnknown, ""};
  std::string error_desc;
  for (const auto& remote : remotes) {
    LOG_INFO << "Fetching ostree commit " + target.Sha256Hash() + " from " + remote.baseUrl;
    if (!remote.isRemoteSet) {
      setRemote(remote.name, remote.baseUrl, remote.keys);
    }
    pull_err = OstreeManager::pull(config.sysroot, remote.baseUrl, keys_, Target::fromTufTarget(target), nullptr,
                                   prog_cb, remote.isRemoteSet ? nullptr : remote.name.c_str(), remote.headers);
    if (pull_err.isSuccess()) {
      res = {DownloadResult::Status::Ok, ""};
      break;
    }

    LOG_ERROR << "Failed to fetch from " + remote.baseUrl + ", err: " + pull_err.description;

    if (  // not enough storage space in the case of a regular pull (pulling objects/files)
        (pull_err.description.find("would be exceeded, at least") != std::string::npos &&
         (pull_err.description.find("min-free-space-size") != std::string::npos ||
          pull_err.description.find("min-free-space-percent") != std::string::npos)) ||
        // not enough storage space in the case of a static delta pull (pulling the delta parts/files)
        (pull_err.description.find("Delta requires") != std::string::npos &&
         pull_err.description.find("free space, but only") != std::string::npos)) {
      res = {DownloadResult::Status::DownloadFailed_NoSpace,
             "Insufficient storage available; path: " + config.sysroot.string() + "; err: " + pull_err.description,
             sysroot_->path()};
      break;
    }
    error_desc += pull_err.description + "\n";
    res = {DownloadResult::Status::DownloadFailed, error_desc};
  }

  return res;
}

bool RootfsTreeManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  (void)target;
  (void)fetcher;
  (void)token;
  (void)progress_cb;
  (void)keys;

  throw std::runtime_error("Using obsolete method of package manager: fetchTarget()");
}

void RootfsTreeManager::setInitialTargetIfNeeded(const std::string& hw_id) {
  const auto current{getCurrent()};
  if (!Target::isUnknown(current)) {
    return;
  }
  try {
    // Turning "unknown" Target to "initial" one
    Uptane::Target init_target{Target::toInitial(current, hw_id)};
    completeInitialTarget(init_target);
    storage_->savePrimaryInstalledVersion(init_target, InstalledVersionUpdateMode::kCurrent);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to set the initial Target: " << exc.what();
  }
}

void RootfsTreeManager::installNotify(const Uptane::Target& target) {
  if (sysroot_->reload()) {
    LOG_DEBUG << "Change in the ostree-based sysroot has been detected after installation;"
              << " booted on: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent)
              << " pending: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending);

  } else {
    LOG_WARNING << "Change in the ostree-based sysroot has NOT been detected after installation;"
                << " booted on: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent)
                << " pending: " << sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kPending);
  }
  OstreeManager::installNotify(target);
}

data::InstallationResult RootfsTreeManager::install(const Uptane::Target& target) const {
  data::InstallationResult res;
  Uptane::Target current = OstreeManager::getCurrent();
  if (current.sha256Hash() != target.sha256Hash() && boot_fw_update_status_->isUpdateSupported()) {
    res = verifyBootloaderUpdate(target);
    if (res.result_code.num_code != data::ResultCode::Numeric::kOk) {
      return res;
    }
  }
  // Do ostree install if the currently installed target's hash differs from the specified target's hash,
  // or there is pending installation and it differs from the specified target so we undeploy it and make the new target
  // pending (app driven rollback)
  if ((current.sha256Hash() != target.sha256Hash()) ||
      (!sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kPending).empty() &&
       sysroot()->getDeploymentHash(OSTree::Sysroot::Deployment::kPending) != target.sha256Hash())) {
    // notify the bootloader before installation happens as it is not atomic
    // and a false notification doesn't hurt with rollback support in place
    // Hacking in order to invoke non-const method from the const one !!!
    const_cast<RootfsTreeManager*>(this)->updateNotify();
    res = OstreeManager::install(target);
    if (res.result_code.num_code == data::ResultCode::Numeric::kInstallFailed) {
      LOG_ERROR << "Failed to install OSTree target";
      return res;
    }
    const_cast<RootfsTreeManager*>(this)->installNotify(target);
    if (current.sha256Hash() == target.sha256Hash() &&
        res.result_code.num_code == data::ResultCode::Numeric::kNeedCompletion) {
      LOG_INFO << "Successfully undeployed the pending failing Target";
      LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
      const_cast<RootfsTreeManager*>(this)->updateNotify();
      res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
    }
  } else {
    LOG_INFO << "Target " << target.sha256Hash() << " is same as current";
    res = data::InstallationResult(data::ResultCode::Numeric::kOk, "OSTree hash already installed, same as current");
  }

  return res;
}

void RootfsTreeManager::getAdditionalRemotes(std::vector<Remote>& remotes, const std::string& target_name) {
  const auto resp = http_client_->post(gateway_url_ + "/download-urls", Json::Value::null);

  if (!resp.isOk()) {
    LOG_WARNING << "Failed to obtain download URLs from Gateway, fallback to dowload via gateway/proxy server: "
                << resp.getStatusStr();
    return;
  }

  const auto respJson = resp.getJson();
  for (Json::ValueConstIterator it = respJson.begin(); it != respJson.end(); it++) {
    remotes.emplace(
        remotes.begin(), Remote{"gcs",
                                (*it)["download_url"].asString(),
                                {
                                    {"X-Correlation-ID", target_name},
                                    {"Authorization", std::string("Bearer ") + (*it)["access_token"].asString()},
                                },
                                boost::none}

    );
  }
}

void RootfsTreeManager::setRemote(const std::string& name, const std::string& url,
                                  const boost::optional<const KeyManager*>& keys) {
  OSTree::Repo repo{sysroot_->path() + "/ostree/repo"};

  if (!!keys) {
    repo.addRemote(name, url, (*keys)->getCaFile(), (*keys)->getCertFile(), (*keys)->getPkeyFile());
  } else {
    repo.addRemote(name, url, "", "", "");
  }
}

data::InstallationResult RootfsTreeManager::verifyBootloaderUpdate(const Uptane::Target& target) const {
  if (update_block_ && boot_fw_update_status_->isUpdateInProgress()) {
    LOG_WARNING << "Bootlader update is in progress."
                   " A device must be rebooted to confirm and finalize the boot fw update"
                   " before installation of a new Target with ostree/rootfs change";
    return data::InstallationResult(data::ResultCode::Numeric::kNeedCompletion, "bootlader update is in progress");
  }

  if (!boot_fw_update_status_->isRollbackProtectionEnabled()) {
    return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
  }

  std::string target_ver_str;
  try {
    target_ver_str = boot_fw_update_status_->getTargetVersion(target.sha256Hash());
  } catch (const std::invalid_argument& exc) {
    // Failure to parse the version file
    LOG_WARNING << "Rejecting the update because a bootloader version file is malformed: " << exc.what();
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, exc.what());
  } catch (const std::exception& exc) {
    LOG_INFO << "Failed to get bootloader version, assuming no bootloader update: " << exc.what();
    return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
  }

  uint64_t target_ver;
  try {
    target_ver = boost::lexical_cast<uint64_t>(target_ver_str);
  } catch (const std::exception& exc) {
    const std::string err_msg{"Invalid format of the bootloader version; value: " + target_ver_str +
                              "; err: " + exc.what()};
    LOG_ERROR << "Rejecting the update since the bootloader version has an invalid format; " << err_msg;
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, exc.what());
  }

  uint64_t cur_ver{0};
  std::string cur_ver_str;
  bool is_current_ver_valid;
  std::tie(cur_ver_str, is_current_ver_valid) = boot_fw_update_status_->getCurrentVersion();
  if (!is_current_ver_valid) {
    LOG_WARNING << "Failed to get current bootloader version: " << cur_ver_str;
    LOG_WARNING << "Assuming that the current bootloader version is `0` and proceeding with the update further";
    cur_ver_str = "0";
  }
  try {
    cur_ver = boost::lexical_cast<uint64_t>(cur_ver_str);
  } catch (const std::exception& exc) {
    LOG_WARNING << "Invalid format of the current bootloader version; value: " << cur_ver_str
                << "; err: " << exc.what();
    LOG_WARNING << "Assuming that the current bootloader version is `0` and proceeding with the update further";
  }

  if (target_ver < cur_ver) {
    const std::string err_msg{"bootloader rollback from version " + cur_ver_str + " to " + target_ver_str +
                              " has been detected"};
    LOG_WARNING << "Rejecting the update because " << err_msg;
    return data::InstallationResult(data::ResultCode::Numeric::kInstallFailed, err_msg);
  }

  return data::InstallationResult(data::ResultCode::Numeric::kOk, "");
}
