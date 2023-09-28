#ifndef AKTUALIZR_LITE_DOCKER_H_
#define AKTUALIZR_LITE_DOCKER_H_

#include <limits>
#include <set>
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
  static Uri parseUri(const std::string& uri, bool factory_app = true);
  Uri createUri(const HashedDigest& digest_in) const;

  const HashedDigest digest;
  const std::string app;
  const std::string factory;
  // This is the <name> field as described in the OCI distribution spec
  // https://github.com/opencontainers/distribution-spec/blob/main/spec.md#pulling-manifests
  // <registryHostname>[:port]/<name>@<digest>
  // <name> == <factory>/<app> in the case of Compose App stored in Fio Registry
  // <name> == <foo> | <foo>/<bar> | <foo>/<bar>/<whatever> - in the case of third party registries
  const std::string repo;  // repo == name
  const std::string registryHostname;
};

struct Descriptor {
  HashedDigest digest;
  int64_t size{0};
  std::string mediaType;

  explicit Descriptor() : digest{"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"} {}
  explicit Descriptor(const Json::Value& value)
      :  // default value is sha256 hash for an empty string
        digest{"sha256:e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855"} {
    static const std::array<std::string, 3> required_fields = {"mediaType", "digest", "size"};
    for (const auto& f : required_fields) {
      if (!value.isMember(f)) {
        throw std::runtime_error("Got invalid descriptor, missing required field; field: " + f +
                                 ", descriptor: " + Utils::jsonToCanonicalStr(value));
      }
    }
    digest = HashedDigest{value["digest"].asString()};
    size = value["size"].asInt64();
    mediaType = value["mediaType"].asString();
  }

  // NOLINTNEXTLINE(hicpp-explicit-conversions,google-explicit-constructor)
  operator bool() const { return !(digest().empty() && size == 0); }
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

  Descriptor layersMetaDescr() const {
    if ((*this)["layers"].size() < 2) {
      LOG_DEBUG << "No layers metadata are found in the App manifest";
      return Descriptor{};
    }
    const auto desc_json{(*this)["layers"][1]};
    if (!desc_json.isMember("annotations") || (!desc_json["annotations"].isMember("layers-meta")) ||
        (desc_json["annotations"]["layers-meta"].asString() != "v1")) {
      LOG_DEBUG << "No layers metadata are found in the App manifest";
      return Descriptor{};
    }
    return Descriptor{desc_json};
  }
};

struct ImageManifest : Json::Value {
  static constexpr const char* const Format{"application/vnd.docker.distribution.manifest.v2+json"};
  static constexpr const char* const Version{"2"};

  explicit ImageManifest(const std::string& json_file) : ImageManifest(Utils::parseJSONFile(json_file)) {}
  explicit ImageManifest(const Json::Value& value);
  Descriptor config() const { return Descriptor{(*this)["config"]}; }
  std::vector<Descriptor> layers() const;
  Json::Value toLoadManifest(const std::string& blobs_dir, const std::vector<std::string>& refs) const;
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

  struct BearerAuth {
    static const std::string Header;
    static const std::string AuthType;
    explicit BearerAuth(const std::string& auth_header_value);

    std::string uri() const { return Realm + "?service=" + Service + "&scope=" + Scope; }

    std::string Realm;
    std::string Service;
    std::string Scope;
  };

  using HttpClientFactory =
      std::function<std::shared_ptr<HttpInterface>(const std::vector<std::string>*, const std::set<std::string>*)>;
  static const HttpClientFactory DefaultHttpClientFactory;
  using Ptr = std::shared_ptr<RegistryClient>;

  explicit RegistryClient(std::shared_ptr<HttpInterface> ota_lite_client,
                          std::string auth_creds_endpoint = DefAuthCredsEndpoint,
                          HttpClientFactory http_client_factory = RegistryClient::DefaultHttpClientFactory);

  std::string getAppManifest(const Uri& uri, const std::string& format,
                             boost::optional<std::int64_t> manifest_size = boost::none) const;
  void downloadBlob(const Uri& uri, const boost::filesystem::path& filepath, size_t expected_size) const;

 private:
  std::string getBasicAuthHeader() const;
  std::string getBearerAuthHeader(const BearerAuth& bearer) const;

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
