#include "composeappengine.h"

namespace Docker {

class RestorableAppEngine : public ComposeAppEngine {
 public:
  static constexpr const char* const SkopeoBin{"skopeo"};

 public:
  RestorableAppEngine(boost::filesystem::path reset_app_root_dir, boost::filesystem::path app_root_dir,
                      std::string compose_bin, Docker::DockerClient::Ptr docker_client,
                      Docker::RegistryClient::Ptr registry_client)
      : ComposeAppEngine(app_root_dir, compose_bin, docker_client, registry_client),
        root_{std::move(reset_app_root_dir)},
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

  bool use_restore_root_;
};

}  // namespace Docker
