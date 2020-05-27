#ifndef COMPOSE_BUNDLES_H_
#define COMPOSE_BUNDLES_H_

#include "package_manager/ostreemanager.h"

#define PACKAGE_MANAGER_COMPOSEAPP "ostree+compose_apps"

// TODO: restructure composeappmanager.* so it contains just the manager implemenation
// ComposeApp and Docker::* stuff should be moved into separate source files

namespace Docker {

struct HashedDigest {
  static const std::string Type;

  HashedDigest(const std::string& hash_digest);

  const std::string& operator()() const { return digest_; }
  const std::string& hash() const { return hash_; }
  const std::string& shortHash() const { return short_hash_; }

 private:
  std::string digest_;
  std::string hash_;
  std::string short_hash_;
};

struct Uri {
  static Uri parseUri(const std::string& uri);
  Uri createUri(const HashedDigest& digest_in);

  const HashedDigest digest;
  const std::string app;
  const std::string factory;
  const std::string repo;
  const std::string registryHostname;
};

class RegistryClient {
 public:
  struct Conf {
    // TODO: consider using Docker's config.json ("auths" and "credHelpers")
    // to make it working against any Docker Registry instance.
    // Currently we just support FIO's Registry so we do shortcut and make direct
    // auth request to the endpoint specified here. We still need this configuration
    // to support the on-premise deployment or deployments under different hostnames
    std::string auth_creds_endpoint{"https://ota-lite.foundries.io:8443/hub-creds/"};
  };

  static const int AuthMaterialMaxSize{1024};
  static const int ManifestMaxSize{2048};
  static const size_t MaxBlobSize{std::numeric_limits<int>::max()};

  static const std::string ManifestEndpoint;
  static const std::string BlobEndpoint;
  static const std::string SupportedRegistryVersion;

  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>(const std::vector<std::string>*)>;
  static HttpClientFactory DefaultHttpClientFactory;

 public:
  RegistryClient(const Conf& conf, const std::shared_ptr<HttpInterface>& ota_lite_client,
                 HttpClientFactory http_client_factory)
      : conf_{conf}, ota_lite_client_{ota_lite_client}, http_client_factory_{std::move(http_client_factory)} {}

  Json::Value getAppManifest(const Uri& uri, const std::string& format) const;

  void downloadBlob(const Uri& uri, const boost::filesystem::path& filepath, size_t expected_size) const;

 private:
  std::string getBasicAuthHeader() const;
  std::string getBearerAuthHeader(const Uri& uri) const;

  static std::string composeManifestUrl(const Uri& uri) {
    return "https://" + uri.registryHostname + SupportedRegistryVersion + uri.repo + ManifestEndpoint + uri.digest();
  }

  static std::string composeBlobUrl(const Uri& uri) {
    return "https://" + uri.registryHostname + SupportedRegistryVersion + uri.repo + BlobEndpoint + uri.digest();
  }

 private:
  const Conf conf_;
  std::shared_ptr<HttpInterface> ota_lite_client_;
  HttpClientFactory http_client_factory_;
};

}  // namespace Docker

class ComposeAppConfig {
 public:
  ComposeAppConfig(const PackageConfig& pconfig);

  std::vector<std::string> apps;
  boost::filesystem::path apps_root;
  boost::filesystem::path compose_bin{"/usr/bin/docker-compose"};
  bool docker_prune{true};

  Docker::RegistryClient::Conf registry_conf;
};

class ComposeAppManager : public OstreeManager {
 public:
  ComposeAppManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    Docker::RegistryClient::HttpClientFactory registry_http_client_factory =
                        Docker::RegistryClient::DefaultHttpClientFactory);

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   FetcherProgressCb progress_cb, const api::FlowControlToken* token) override;
  data::InstallationResult install(const Uptane::Target& target) const override;
  std::string name() const override { return PACKAGE_MANAGER_COMPOSEAPP; };

 private:
  // TODO: consider avoiding it
  FRIEND_TEST(ComposeApp, getApps);
  FRIEND_TEST(ComposeApp, handleRemovedApps);
  FRIEND_TEST(ComposeApp, fetch);

  std::vector<std::pair<std::string, std::string>> getApps(const Uptane::Target& t) const;
  void handleRemovedApps(const Uptane::Target& target) const;

  ComposeAppConfig cfg_;
  Docker::RegistryClient registry_client_;
};

#endif  // COMPOSE_BUNDLES_H_
