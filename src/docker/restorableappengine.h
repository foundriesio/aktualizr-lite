#include "composeappengine.h"
#include "appstore.h"


namespace Docker {

class RestorableAppEngine : public ComposeAppEngine {
 public:
  RestorableAppEngine(boost::filesystem::path reset_app_root_dir, boost::filesystem::path app_root_dir,
                      std::string compose_bin, DockerClient::Ptr docker_client,
                      RegistryClient::Ptr registry_client, AppStore::Ptr app_store)
      : ComposeAppEngine(app_root_dir, compose_bin, docker_client, registry_client),
        app_store_{app_store},
        use_restore_root_{false} {}

 private:
  bool download(const App& app) override;
  bool verify(const App& app) override;
  bool pullImages(const App& app) override;
  bool installApp(const App& app) override;
  bool start(const App& app) override;
  boost::filesystem::path appRoot(const App& app) const override;

  bool installAppImages(const App& app);

 private:
  AppStore::Ptr app_store_;
  bool use_restore_root_;
};

}  // namespace Docker
