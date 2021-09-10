#ifndef AKTUALIZR_LITE_DOCKER_APP_STORE_H_
#define AKTUALIZR_LITE_DOCKER_APP_STORE_H_

#include <memory>
#include <string>
#include <unordered_set>

#include <boost/filesystem.hpp>

#include "appengine.h"
#include "docker/docker.h"

namespace Docker {

class AppStore {
 public:
  using Ptr = std::shared_ptr<AppStore>;

 public:
  AppStore(boost::filesystem::path root, Docker::RegistryClient::Ptr registry_client);

 public:
  virtual boost::filesystem::path appRoot(const AppEngine::App& app) const;
  virtual void pullApp(const AppEngine::App& app);
  virtual bool pullAppImage(const AppEngine::App& app, const std::string& uri, const std::string& auth) const = 0;

  virtual void copyApp(const AppEngine::App& app, const boost::filesystem::path& dst) const;

  virtual bool copyAppImageToDockerStore(const AppEngine::App& app, const std::string& uri) const = 0;
  virtual void purge(const AppEngine::Apps& app_shortlist) const = 0;

 public:
  const boost::filesystem::path& appsRoot() const { return apps_root_; }
  const boost::filesystem::path& blobsRoot() const { return blobs_root_; }

 protected:
  static bool runCmd(const std::string& cmd, const boost::filesystem::path& dir = "");

 protected:
  const boost::filesystem::path root_;
  const boost::filesystem::path apps_root_{root_ / "apps"};
  const boost::filesystem::path blobs_root_{root_ / "blobs"};

  Docker::RegistryClient::Ptr registry_client_;
};

class SkopeoAppStore : public AppStore {
 public:
  static constexpr const char* const ManifestFormat{"v2s2"};
  SkopeoAppStore(std::string skopeo_bin, boost::filesystem::path root, Docker::RegistryClient::Ptr registry_client);

 public:
  bool pullAppImage(const AppEngine::App& app, const std::string& uri, const std::string& auth) const override;
  bool copyAppImageToDockerStore(const AppEngine::App& app, const std::string& uri) const override;
  void purge(const AppEngine::Apps& app_shortlist) const override;

 protected:
  boost::filesystem::path getAppImageRoot(const AppEngine::App& app, const std::string& uri) const;
  void purgeApps(const AppEngine::Apps& app_shortlist, std::unordered_set<std::string>& blob_shortlist) const;
  void purgeBlobs(const std::unordered_set<std::string>& blob_shortlist) const;

 private:
  const std::string skopeo_bin_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_APP_STORE_H_
