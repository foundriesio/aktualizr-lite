#ifndef AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
#define AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_

#include "docker.h"
#include "ostree.h"
#include "package_manager/ostreemanager.h"

class ComposeAppManager : public OstreeManager {
 public:
  static constexpr const char* const Name{"ostree+compose_apps"};

  struct Config {
   public:
    Config(const PackageConfig& pconfig);

    std::vector<std::string> apps;
    boost::filesystem::path apps_root;
    boost::filesystem::path compose_bin{"/usr/bin/docker-compose"};
    bool docker_prune{true};
  };

  ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot,
                    Docker::RegistryClient::HttpClientFactory registry_http_client_factory =
                        Docker::RegistryClient::DefaultHttpClientFactory);

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   FetcherProgressCb progress_cb, const api::FlowControlToken* token) override;
  data::InstallationResult install(const Uptane::Target& target) const override;
  std::string name() const override { return Name; };

  std::vector<std::pair<std::string, std::string>> getApps(const Uptane::Target& t) const;
  void handleRemovedApps(const Uptane::Target& target) const;
  std::string getCurrentHash() const override;

 private:
  Config cfg_;
  std::shared_ptr<OSTree::Sysroot> sysroot_;
  Docker::RegistryClient registry_client_;
  const std::string compose_bin_;
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_MANAGER_H_
