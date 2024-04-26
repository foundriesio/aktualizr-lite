#ifndef AKTUALIZR_LITE_COMPOSEAPP_APP_ENGINE_H
#define AKTUALIZR_LITE_COMPOSEAPP_APP_ENGINE_H

#include "docker/restorableappengine.h"

namespace composeapp {

class AppEngine : public Docker::RestorableAppEngine {
 public:
  AppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
            boost::filesystem::path docker_root, Docker::RegistryClient::Ptr registry_client,
            Docker::DockerClient::Ptr docker_client, std::string docker_host = "unix:///var/run/docker.sock",
            std::string compose_cmd = "/usr/bin/docker-compose", std::string composectl_cmd = "/usr/bin/composectl",
            int storage_watermark = 80,
            StorageSpaceFunc storage_space_func = RestorableAppEngine::GetDefStorageSpaceFunc(),
            ClientImageSrcFunc client_image_src_func = nullptr, bool create_containers_if_install = true,
            const std::string& local_source_path = "")
      : Docker::RestorableAppEngine(
            std::move(store_root), std::move(install_root), std::move(docker_root), std::move(registry_client),
            std::move(docker_client), "", std::move(docker_host), std::move(compose_cmd), std::move(storage_space_func),
            std::move(client_image_src_func), create_containers_if_install, !local_source_path.empty()),
        composectl_cmd_{std::move(composectl_cmd)},
        storage_watermark_{storage_watermark},
        local_source_path_{local_source_path} {}

  Result fetch(const App& app) override;

 private:
  bool isAppFetched(const App& app) const override;
  bool isAppInstalled(const App& app) const override;
  void installAppAndImages(const App& app) override;

  const std::string composectl_cmd_;
  const int storage_watermark_;
  const std::string local_source_path_;
};

}  // namespace composeapp

#endif  // AKTUALIZR_LITE_COMPOSEAPP_APP_ENGINE_H
