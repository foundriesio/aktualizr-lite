#ifndef COMPOSE_BUNDLES_H_
#define COMPOSE_BUNDLES_H_

#include "package_manager/ostreemanager.h"

#define PACKAGE_MANAGER_COMPOSEAPP "ostree+compose_apps"

class RegistryClient {
 public:
  struct Conf {
    std::string auth_creds_endpoint{"https://ota-lite.foundries.io:8443/hub-creds/"};
    std::string base_url{"https://hub.foundries.io"};
    std::string auth_token_endpoint{base_url + "/token-auth/"};
    std::string repo_base_url{base_url + "/v2/"};
  };

  static const int AuthMaterialMaxSize{1024};
  static const int ManifestMaxSize{2048};
  static const size_t MaxBlobSize{std::numeric_limits<int>::max()};

  struct HashedDigest {
    static const std::string Type;
    HashedDigest(const std::string& hash_digest = Type + ':');

    const std::string& operator()() const { return digest_; }
    const std::string& hash() const { return hash_; }
    const std::string& shortHash() const { return short_hash_; }

   private:
    std::string digest_;
    std::string hash_;
    std::string short_hash_;
  };

  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>(const std::vector<std::string>*)>;

  static HttpClientFactory DefaultHttpClientFactory;

 public:
  RegistryClient(const Conf& conf, const std::shared_ptr<HttpInterface>& ota_lite_client,
                 HttpClientFactory http_client_factory)
      : conf_{conf}, ota_lite_client_{ota_lite_client}, http_client_factory_{std::move(http_client_factory)} {}

  Json::Value getAppManifest(const std::string& repo, const HashedDigest& digest, const std::string& format) const;

  void downloadBlob(const std::string& repo, const HashedDigest& digest, const boost::filesystem::path& file_path,
                    size_t expected_size) const;

 private:
  std::string getBasicAuthHeader() const;
  std::string getBearerAuthHeader(const std::string& repo) const;

 private:
  const Conf conf_;
  std::shared_ptr<HttpInterface> ota_lite_client_;
  HttpClientFactory http_client_factory_;
};

class ComposeAppConfig {
 public:
  ComposeAppConfig(const PackageConfig& pconfig);

  std::vector<std::string> apps;
  boost::filesystem::path apps_root;
  boost::filesystem::path compose_bin{"/usr/bin/docker-compose"};
  bool docker_prune{true};

  RegistryClient::Conf registry_conf;
};

class ComposeAppManager : public OstreeManager {
 public:
  ComposeAppManager(
      const PackageConfig& pconfig, const BootloaderConfig& bconfig, const std::shared_ptr<INvStorage>& storage,
      const std::shared_ptr<HttpInterface>& http,
      RegistryClient::HttpClientFactory registry_http_client_factory = RegistryClient::DefaultHttpClientFactory);

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   FetcherProgressCb progress_cb, const api::FlowControlToken* token) override;
  data::InstallationResult install(const Uptane::Target& target) const override;
  std::string name() const override { return PACKAGE_MANAGER_COMPOSEAPP; };

 private:
  // TODO: conssider avoiding it
  FRIEND_TEST(ComposeApp, getApps);
  FRIEND_TEST(ComposeApp, handleRemovedApps);
  FRIEND_TEST(ComposeApp, fetch);

  std::vector<std::pair<std::string, std::string>> getApps(const Uptane::Target& t) const;
  void handleRemovedApps(const Uptane::Target& target) const;

  ComposeAppConfig cfg_;
  RegistryClient registry_client_;
};

#endif  // COMPOSE_BUNDLES_H_
