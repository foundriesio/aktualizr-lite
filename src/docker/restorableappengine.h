#ifndef AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
#define AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_

#include "appengine.h"

#include <functional>

#include "docker/docker.h"
#include "docker/dockerclient.h"

namespace Docker {

/**
 * @brief RestorableAppEngine, implementation of App Engine that can reset or restore Apps in case of docker engine
 * failure
 *
 *
 * PackageManagerInterface::fetchTarget()
 * - Pulls Apps from Registries by using `skopeo` and store Apps' content under the folder defined in
 * sota.toml:[pacman].reset_apps_root param
 *
 * PackageManagerInterface::install()
 * - Copy Apps' content from sota.toml:[pacman].reset_apps_root to sota.toml:[pacman].compose_apps_root and a docker
 * store;
 * - Bring Apps up, `docker-compose up [--no-start]` for each App dir in sota.toml:[pacman].compose_apps_root;
 *
 *
 * Store layout
 *
 * sota.toml:[pacman].reset_apps_root/
 *      apps/
 *        <app-name>/
 *          <app-hash>/
 *            manifest.json (refers to the App archive)
 *            <app-archive-hash>.tgz (contains docker-compose.yml and accompanied files)
 *            /images/
 *              <registry-host-name>/
 *                <factory|org>/
 *                  <repo|image-name>/
 *                    <image-hash>/
 *                      index.json (refers to the image manifest blob)
 *                      oci-layout
 *
 *
 *      blobs/
 *        sha256/ (it contains manifest, container config and layer blobs)
 *          <blob-01>
 *          ...
 *          <blob-N>
 *
 *
 * Compose App dir layout
 *
 * sota.toml:[pacman].compose_apps_root/
 *   <app-name>/ (content of App archive is extracted into this directory)
 *     docker-compose.yml
 *     [optional App additionl files]
 *
 *
 * Docker image&layer store layout
 *
 * <docker-data-root>/
 *   image/
 *     overlay2/
 *       repositories.json (`docker images` entry point, maps an image URI to the images present in the store)
 *       imagedb/
 *         metadata/ (engine specific metadata about images)
 *         content/ (image manifests, each image config and its layers)
 *       layerdb/ (references on the extracted/unpacked layer filesystems located under <docker-data-root>/overlay2) as
 * well as R/W top-layer mounts
 *
 */

class RestorableAppEngine : public AppEngine {
 public:
  static const std::string ComposeFile;
  using StorageSpaceFunc =
      std::function<std::tuple<boost::uintmax_t, boost::uintmax_t>(const boost::filesystem::path&)>;

  static const int LowWatermarkLimit{20};
  static const int HighWatermarkLimit{95};
  static StorageSpaceFunc GetDefStorageSpaceFunc(int watermark = 80);

  RestorableAppEngine(boost::filesystem::path store_root, boost::filesystem::path install_root,
                      boost::filesystem::path docker_root, Docker::RegistryClient::Ptr registry_client,
                      Docker::DockerClient::Ptr docker_client, std::string client = "/sbin/skopeo",
                      std::string docker_host = "unix:///var/run/docker.sock",
                      std::string compose_cmd = "/usr/bin/docker-compose",
                      StorageSpaceFunc storage_space_func = RestorableAppEngine::GetDefStorageSpaceFunc());

  Result fetch(const App& app) override;
  Result verify(const App& app) override;
  Result install(const App& app) override;
  Result run(const App& app) override;
  void remove(const App& app) override;
  bool isFetched(const App& app) const override;
  bool isRunning(const App& app) const override;
  Json::Value getRunningAppsInfo() const override;
  void prune(const Apps& app_shortlist) override;

 private:
  // pull App&Images
  void pullApp(const Uri& uri, const boost::filesystem::path& app_dir);
  void checkAppUpdateSize(const Uri& uri, const boost::filesystem::path& app_dir) const;
  void pullAppImages(const boost::filesystem::path& app_compose_file, const boost::filesystem::path& dst_dir);

  // install App&Images
  boost::filesystem::path installAppAndImages(const App& app);
  static void installApp(const boost::filesystem::path& app_dir, const boost::filesystem::path& dst_dir);
  void installAppImages(const boost::filesystem::path& app_dir);

  bool isAppFetched(const App& app) const;
  bool areAppImagesFetched(const App& app) const;
  bool isAppInstalled(const App& app) const;

  // check if App&Images are running
  static bool isRunning(const App& app, const std::string& compose_file,
                        const Docker::DockerClient::Ptr& docker_client);

  static bool areContainersCreated(const App& app, const std::string& compose_file,
                                   const Docker::DockerClient::Ptr& docker_client);
  static bool checkAppContainers(const App& app, const std::string& compose_file,
                                 const Docker::DockerClient::Ptr& docker_client, bool check_state = true);

  // functions specific to an image tranfer utility
  static void pullImage(const std::string& client, const std::string& uri, const boost::filesystem::path& dst_dir,
                        const boost::filesystem::path& shared_blob_dir, const std::string& format = "v2s2");

  static void installImage(const std::string& client, const boost::filesystem::path& image_dir,
                           const boost::filesystem::path& shared_blob_dir, const std::string& docker_host,
                           const std::string& tag, const std::string& format = "v2s2");

  static void verifyComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir);
  static void startComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir,
                              const std::string& flags = "up --remove-orphans -d");

  static void stopComposeApp(const std::string& compose_cmd, const boost::filesystem::path& app_dir);
  static std::string getContentHash(const boost::filesystem::path& path);

  static uint64_t getAppUpdateSize(const Json::Value& app_layers, const boost::filesystem::path& blob_dir);
  static uint64_t getDockerStoreSizeForAppUpdate(const uint64_t& compressed_update_size,
                                                 uint32_t average_compression_ratio);

  void checkAvailableStorageInStores(const std::string& app_name, const uint64_t& skopeo_required_storage,
                                     const uint64_t& docker_required_storage) const;

  static bool areDockerAndSkopeoOnTheSameVolume(const boost::filesystem::path& skopeo_path,
                                                const boost::filesystem::path& docker_path);
  static std::tuple<uint64_t, bool> getPathVolumeID(const boost::filesystem::path& path);

  const boost::filesystem::path store_root_;
  const boost::filesystem::path install_root_;
  const boost::filesystem::path docker_root_;
  const bool docker_and_skopeo_same_volume_{true};
  const std::string client_;
  const std::string docker_host_;
  const std::string compose_cmd_;
  const boost::filesystem::path apps_root_{store_root_ / "apps"};
  const boost::filesystem::path blobs_root_{store_root_ / "blobs"};
  Docker::RegistryClient::Ptr registry_client_;
  Docker::DockerClient::Ptr docker_client_;
  StorageSpaceFunc storage_space_func_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_RESTORABLE_APP_ENGINE_H_
