#include "rootfstreemanager.h"
#include "ostree/repo.h"
#include "target.h"

DownloadResult RootfsTreeManager::Download(const TufTarget& target) {
  if (!checkTargetInsufficientStorageLevelIfSet(target.Name())) {
    const std::string err_msg{
        "Skip Target download, available storage has not been increased since Target download failed last time due to "
        "lack of space"};
    LOG_ERROR << err_msg;
    return {DownloadResult::Status::DownloadFailed_NoSpace,
            "Insufficient storage available; path: " + config.sysroot.string() + "; err: " + err_msg};
  }

  auto prog_cb = [this](const Uptane::Target& t, const std::string& description, unsigned int progress) {
    // report_progress_cb(events_channel.get(), t, description, progress);
    // TODO: consider make use of it for download progress reporting
  };

  std::vector<Remote> remotes = {{remote, config.ostree_server, {{"X-Correlation-ID", target.Name()}}, &keys_, false}};

  getAdditionalRemotes(remotes, target.Name());

  DownloadResult res{DownloadResult::Status::Ok, ""};
  data::InstallationResult pull_err{data::ResultCode::Numeric::kUnknown, ""};
  std::string error_desc;
  for (const auto& remote : remotes) {
    LOG_INFO << "Fetchig ostree commit " + target.Sha256Hash() + " from " + remote.baseUrl;
    if (!remote.isRemoteSet) {
      setRemote(remote.name, remote.baseUrl, remote.keys);
    }
    pull_err = OstreeManager::pull(config.sysroot, remote.baseUrl, keys_, Target::fromTufTarget(target), nullptr,
                                   prog_cb, remote.isRemoteSet ? nullptr : remote.name.c_str(), remote.headers);
    if (pull_err.isSuccess()) {
      res = {DownloadResult::Status::Ok, ""};
      unsetTargetInsufficientStorageLevel(target.Name());
      break;
    }

    LOG_ERROR << "Failed to fetch from " + remote.baseUrl + ", err: " + pull_err.description;

    if (pull_err.description.find("would be exceeded, at least") != std::string::npos &&
        (pull_err.description.find("min-free-space-size") != std::string::npos ||
         pull_err.description.find("min-free-space-percent") != std::string::npos)) {
      setTargetInsufficientStorageLevel(target.Name());
      res = {DownloadResult::Status::DownloadFailed_NoSpace,
             "Insufficient storage available; path: " + config.sysroot.string() + "; err: " + pull_err.description};
      break;
    }
    unsetTargetInsufficientStorageLevel(target.Name());
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

void RootfsTreeManager::setTargetInsufficientStorageLevel(const std::string& target_name) {
  boost::system::error_code ec;
  const boost::filesystem::space_info store_info{boost::filesystem::space(sysroot_->path(), ec)};
  if (ec.failed()) {
    LOG_WARNING << "Failed to obtain info about available storage: " << ec.message();
    return;
  }

  try {
    const auto available_storage_size{std::to_string(store_info.available)};
    OSTree::Repo repo{sysroot_->path() + "/ostree/repo"};
    repo.setConfigItem("min-free-space-required", target_name, available_storage_size);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to set Target insufficient storage level: " << exc.what();
  }
}

bool RootfsTreeManager::checkTargetInsufficientStorageLevelIfSet(const std::string& target_name) {
  try {
    OSTree::Repo repo{sysroot_->path() + "/ostree/repo"};
    const std::string required_min_space_str{repo.getConfigItem("min-free-space-required", target_name)};
    if (required_min_space_str.empty()) {
      return true;
    }
    const boost::uintmax_t required_min_space{boost::lexical_cast<boost::uintmax_t>(required_min_space_str)};

    boost::system::error_code ec;
    const boost::filesystem::space_info store_info{boost::filesystem::space(sysroot_->path(), ec)};
    if (ec.failed()) {
      LOG_WARNING << "Failed to obtain info about available storage: " << ec.message();
      return true;
    }

    const auto exp_required_min_space{required_min_space + (4 * 1024) /* at least 4K change */};
    LOG_INFO << "Target " << target_name << " needs at least " << exp_required_min_space << " of free space, got "
             << store_info.available;
    return store_info.available > exp_required_min_space;
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to check Target insufficient storage level: " << exc.what();
  }
  return true;
}

void RootfsTreeManager::unsetTargetInsufficientStorageLevel(const std::string& target_name) {
  try {
    OSTree::Repo repo{sysroot_->path() + "/ostree/repo"};
    repo.unsetConfigItem("min-free-space-required", target_name);
  } catch (const std::exception& exc) {
    LOG_ERROR << "Failed to unset Target insufficient storage level: " << exc.what();
  }
}
