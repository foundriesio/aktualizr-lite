#ifndef AKTUALIZR_LITE_CONTAINERD_ENGINE_H
#define AKTUALIZR_LITE_CONTAINERD_ENGINE_H

#include "docker/composeappengine.h"

namespace containerd {

class Engine : public Docker::ComposeAppEngine {
 public:
  Engine(boost::filesystem::path root_dir, std::string compose_bin, AppEngine::Client::Ptr client,
         Docker::RegistryClient::Ptr registry_client)
      : Docker::ComposeAppEngine(std::move(root_dir), std::move(compose_bin), std::move(client),
                                 std::move(registry_client)) {}

 private:
  void pullImages(const App& app) override;
  void installApp(const App& app) override;
  void runComposeCmd(const App& app, const std::string& cmd, const std::string& err_msg) const override;
};

}  // namespace containerd

#endif  // AKTUALIZR_LITE_CONTAINERD_ENGINE_H
