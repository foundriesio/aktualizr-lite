#include "rootfstreemanager.h"
#include "ostree/repo.h"

bool RootfsTreeManager::fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                                    const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) {
  (void)fetcher;

  std::vector<Remote> remotes = {
      {remote, config.ostree_server, {{"X-Correlation-ID", target.filename()}}, &keys, false}};

  getAdditionalRemotes(remotes, target.filename());

  data::InstallationResult pull_err{data::ResultCode::Numeric::kUnknown, ""};
  for (const auto& remote : remotes) {
    LOG_INFO << "Fetchig ostree commit " + target.sha256Hash() + " from " + remote.baseUrl;
    if (!remote.isRemoteSet) {
      setRemote(remote.name, remote.baseUrl, remote.keys);
    }
    pull_err = OstreeManager::pull(config.sysroot, remote.baseUrl, keys, target, token, progress_cb,
                                   remote.isRemoteSet ? nullptr : remote.name.c_str(), remote.headers);
    if (pull_err.isSuccess()) {
      break;
    }
    LOG_INFO << "Failed to fetch from " + remote.baseUrl + ", err: " + pull_err.description;
  }

  if (!pull_err.isSuccess()) {
    LOG_ERROR << "Failed to fetch ostree commit " + target.sha256Hash() + ", err: " + pull_err.description;
  }

  return pull_err.isSuccess();
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
