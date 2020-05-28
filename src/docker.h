#ifndef AKTUALIZR_LITE_DOCKER_H_
#define AKTUALIZR_LITE_DOCKER_H_

#include <limits>
#include <string>

#include <http/httpinterface.h>

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
  static constexpr const char* const DefAuthCredsEndpoint{"https://ota-lite.foundries.io:8443/hub-creds/"};
  static const int AuthMaterialMaxSize{1024};
  static const int ManifestMaxSize{2048};
  static const size_t MaxBlobSize{std::numeric_limits<int>::max()};

  static const std::string ManifestEndpoint;
  static const std::string BlobEndpoint;
  static const std::string SupportedRegistryVersion;

  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>(const std::vector<std::string>*)>;
  static HttpClientFactory DefaultHttpClientFactory;

 public:
  RegistryClient(const std::string& treehub_endpoint, const std::shared_ptr<HttpInterface>& ota_lite_client,
                 HttpClientFactory http_client_factory);

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
  std::string auth_creds_endpoint_;
  std::shared_ptr<HttpInterface> ota_lite_client_;
  HttpClientFactory http_client_factory_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_H_
