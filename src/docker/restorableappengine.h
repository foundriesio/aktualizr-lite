#ifndef AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
#define AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_

#include "appengine.h"

#include "docker/docker.h"

namespace Docker {

class RestorableAppEngine : public AppEngine {
 public:
  static const std::string ComposeFile;

 public:
  RestorableAppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
                      Docker::RegistryClient::Ptr registry_client, const std::string& client = "skopeo",
                      const std::string& docker_host = "unix:///var/run/docker.sock");

 public:
  bool fetch(const App& app) override;
  bool install(const App& app) override;
  bool run(const App& app) override;
  void remove(const App& app) override;
  bool isRunning(const App& app) const override;
  Json::Value getRunningAppsInfo() const override;

 private:
  boost::filesystem::path pullApp(const Uri& uri, const boost::filesystem::path& app_dir);
  void pullAppImages(const boost::filesystem::path& app_compose_file, const boost::filesystem::path& dst_dir);

  void installApp(const boost::filesystem::path& app_dir, const boost::filesystem::path& dst_dir);
  void installAppImages(const boost::filesystem::path& app_dir);

  // functions specific to an image tranfer utility
  static void pullImage(const std::string& client, const std::string& uri, const boost::filesystem::path& dst_dir,
                        const boost::filesystem::path& shared_blob_dir, const std::string& format = "v2s2");

  static void installImage(const std::string& client, const boost::filesystem::path& image_dir,
                           const boost::filesystem::path& shared_blob_dir, const std::string& docker_host,
                           const std::string& tag, const std::string& format = "v2s2");

 private:
  const boost::filesystem::path store_root_;
  const boost::filesystem::path install_root_;
  const std::string client_;
  const std::string docker_host_;
  const boost::filesystem::path apps_root_{store_root_ / "apps"};
  const boost::filesystem::path blobs_root_{store_root_ / "blobs"};
  Docker::RegistryClient::Ptr registry_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
