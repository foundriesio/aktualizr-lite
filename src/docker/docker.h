#ifndef AKTUALIZR_LITE_DOCKER_H_
#define AKTUALIZR_LITE_DOCKER_H_

#include <limits>
#include <string>

#include <boost/optional.hpp>

#include <http/httpinterface.h>

namespace Docker {

struct HashedDigest {
  static const std::string Type;

  explicit HashedDigest(const std::string& hash_digest);

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
  Uri createUri(const HashedDigest& digest_in) const;

  const HashedDigest digest;
  const std::string app;
  const std::string factory;
  const std::string repo;
  const std::string registryHostname;
};

struct Manifest : Json::Value {
  static constexpr const char* const Format{"application/vnd.oci.image.manifest.v1+json"};
  static constexpr const char* const IndexFormat{"application/vnd.oci.image.index.v1+json"};
  static constexpr const char* const Version{"v1"};
  static constexpr const char* const ArchiveExt{".tgz"};
  static constexpr const char* const Filename{"manifest.json"};

  explicit Manifest(const std::string& json_str) : Manifest(Utils::parseJSON(json_str)) {}
  explicit Manifest(const Json::Value& value = Json::Value()) : Json::Value(value) {
    auto manifest_version{(*this)["annotations"]["compose-app"].asString()};
    if (manifest_version.empty()) {
      throw std::runtime_error("Got invalid App manifest, missing a manifest version: " +
                               Utils::jsonToCanonicalStr(value));
    }
    if (Version != manifest_version) {
      throw std::runtime_error("Got unsupported App manifest version: " + Utils::jsonToCanonicalStr(value));
    }
  }

  std::string archiveDigest() const {
    auto digest{(*this)["layers"][0]["digest"].asString()};
    if (digest.empty()) {
      throw std::runtime_error("Got invalid App manifest, failed to extract App Archive digest from App manifest: " +
                               Utils::jsonToCanonicalStr(*this));
    }
    return digest;
  }

  size_t archiveSize() const {
    uint64_t arch_size{(*this)["layers"][0]["size"].asUInt64()};
    if (0 == arch_size || arch_size > std::numeric_limits<size_t>::max()) {
      throw std::runtime_error("Invalid size of App Archive is specified in App manifest: " +
                               Utils::jsonToCanonicalStr(*this));
    }

    return arch_size;
  }

  Json::Value layersManifest(const std::string& arch) const {
    const Json::Value manifests{(*this)["manifests"]};
    if (!manifests.isArray()) {
      LOG_WARNING << "App manifest doesn't include layers manifests";
      return Json::Value();
    }

    for (Json::ValueConstIterator ii = manifests.begin(); ii != manifests.end(); ++ii) {
      const auto man_arch{(*ii)["platform"]["architecture"].asString()};
      if (arch == man_arch) {
        return *ii;
      }
    }

    LOG_WARNING << "App manifest doesn't include a layers manifest of a given architecture: " << arch;
    return Json::Value();
  }
};

class RegistryClient {
 public:
  static constexpr const char* const DefAuthCredsEndpoint{"https://ota-lite.foundries.io:8443/hub-creds/"};
  static const int AuthMaterialMaxSize{1024};
  static const int DefManifestMaxSize{16384};
  static const size_t MaxBlobSize{std::numeric_limits<int>::max()};

  static const std::string ManifestEndpoint;
  static const std::string BlobEndpoint;
  static const std::string SupportedRegistryVersion;

  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>(const std::vector<std::string>*)>;
  static HttpClientFactory DefaultHttpClientFactory;
  using Ptr = std::shared_ptr<RegistryClient>;

  explicit RegistryClient(std::shared_ptr<HttpInterface> ota_lite_client,
                          std::string auth_creds_endpoint = DefAuthCredsEndpoint,
                          HttpClientFactory http_client_factory = RegistryClient::DefaultHttpClientFactory);

  std::string getAppManifest(const Uri& uri, const std::string& format,
                             boost::optional<std::int64_t> manifest_size = boost::none) const;
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

  const std::string auth_creds_endpoint_;
  std::shared_ptr<HttpInterface> ota_lite_client_;
  HttpClientFactory http_client_factory_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_H_
