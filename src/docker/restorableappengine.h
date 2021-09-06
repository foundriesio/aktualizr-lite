#include "composeappengine.h"

namespace Docker {

struct SkopeoCmd {
  static constexpr const char* const ManifestFormat{"v2s2"};
  SkopeoCmd(std::string skopeo_bin):skopeo_bin_{std::move(skopeo_bin)} {}

  static bool run_cmd(const std::string& cmd);

  bool pullFromRegistry(const std::string& srs, const std::string& dst_images, const std::string& dst_blobs, const std::string& auth = "");
  bool copyToDockerStore(const std::string& srs_image, const std::string& src_blobs, const std::string& dst);

  const std::string skopeo_bin_;

};

class RestorableAppEngine : public ComposeAppEngine {
 public:
  RestorableAppEngine(boost::filesystem::path reset_app_root_dir, boost::filesystem::path app_root_dir,
                      std::string compose_bin, std::string skopeo_bin, Docker::DockerClient::Ptr docker_client,
                      Docker::RegistryClient::Ptr registry_client)
      : ComposeAppEngine(app_root_dir, compose_bin, docker_client, registry_client),
        root_{std::move(reset_app_root_dir)},
        skopeo_cmd_{SkopeoCmd{std::move(skopeo_bin)}},
        use_restore_root_{false} {}

 private:
  bool download(const App& app) override;
  bool verify(const App& app) override;
  bool pullImages(const App& app) override;
  bool installApp(const App& app) override;
  bool start(const App& app) override;
  boost::filesystem::path appRoot(const App& app) const override;

  bool pullImagesWithSkopeo(const App& app);
  bool pullImagesWithDocker(const App& app);

  bool installAppWithDocker(const App& app);
  bool installAppWithSkopeo(const App& app);

 private:
  const boost::filesystem::path root_;
  const boost::filesystem::path apps_root_{root_ / "apps"};
  const boost::filesystem::path images_root_{root_ / "images"};
  const boost::filesystem::path images_blobs_root_{root_ / "images" / "blobs"};
  SkopeoCmd skopeo_cmd_;

  bool use_restore_root_;
};

}  // namespace Docker
