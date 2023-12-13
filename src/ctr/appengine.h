#ifndef AKTUALIZR_LITE_CTR_APP_ENGINE_H
#define AKTUALIZR_LITE_CTR_APP_ENGINE_H

#include "docker/restorableappengine.h"

namespace ctr {

class AppEngine : public Docker::RestorableAppEngine {
 public:
  AppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
            boost::filesystem::path docker_root, Docker::RegistryClient::Ptr registry_client,
            Docker::DockerClient::Ptr docker_client, std::string client = "/sbin/skopeo",
            std::string docker_host = "unix:///var/run/docker.sock",
            std::string compose_cmd = "/usr/bin/docker-compose", std::string composectl_cmd = "/usr/bin/composectl",
            int storage_watermark = 80,
            StorageSpaceFunc storage_space_func = RestorableAppEngine::GetDefStorageSpaceFunc(),
            ClientImageSrcFunc client_image_src_func = nullptr, bool create_containers_if_install = true,
            std::string local_source_path = "")
      : Docker::RestorableAppEngine(store_root, install_root, docker_root, registry_client, docker_client, client,
                                    docker_host, compose_cmd, storage_space_func, client_image_src_func,
                                    create_containers_if_install, !local_source_path.empty()),
        composectl_cmd_{composectl_cmd},
        storage_watermark_{storage_watermark},
        local_source_path_{local_source_path} {}

  Result fetch(const App& app) override;

 private:
  void installAppAndImages(const App& app) override;

 private:
  const std::string composectl_cmd_;
  const int storage_watermark_;
  const std::string local_source_path_;
};

}  // namespace ctr

#endif  // AKTUALIZR_LITE_CTR_APP_ENGINE_H
