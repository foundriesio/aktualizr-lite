#ifndef AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
#define AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_

#include "appengine.h"

#include "docker/docker.h"

namespace Docker {

class RestorableAppEngine : public AppEngine {
 public:
  static const std::string ComposeFile;

 public:
  RestorableAppEngine(boost::filesystem::path store_root, Docker::RegistryClient::Ptr registry_client,
                      const std::string& client = "skopeo");

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

  // image tranfer utility specific functions
  static void pullImage(const std::string& client, const std::string& uri, const boost::filesystem::path& dst_dir,
                        const boost::filesystem::path& shared_blob_dir, const std::string& format = "v2s2");

 private:
  const boost::filesystem::path store_root_;
  const std::string client_;
  const boost::filesystem::path apps_root_{store_root_ / "apps"};
  const boost::filesystem::path blobs_root_{store_root_ / "blobs"};
  Docker::RegistryClient::Ptr registry_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
