#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "http/httpclient.h"
#include "uptane/imagerepository.h"

#include "akhttpsreposource.h"
#include "crypto/p11engine.h"

#ifdef BUILD_P11
static constexpr bool built_with_p11 = true;
#else
static constexpr bool built_with_p11 = false;
#endif

namespace aklite::tuf {

AkHttpsRepoSource::AkHttpsRepoSource(const std::string& name_in, boost::property_tree::ptree& pt) {
  boost::program_options::variables_map m;
  Config config(m);
  fillConfig(config, pt);
  init(name_in, pt, config);
}

AkHttpsRepoSource::AkHttpsRepoSource(const std::string& name_in, boost::property_tree::ptree& pt, Config& config) {
  init(name_in, pt, config);
}

static std::string readFileIfExists(const utils::BasedPath& based_path) {
  if (based_path.empty()) {
    return "";
  }
  boost::filesystem::path path = based_path.get("");
  if (boost::filesystem::exists(path)) {
    return Utils::readFile(path);
  } else {
    return "";
  }
}

void AkHttpsRepoSource::init(const std::string& name_in, boost::property_tree::ptree& pt, Config& config) {
  name_ = name_in;

  std::vector<std::string> headers;
  headers.emplace_back("x-ats-tags: " + Utils::stripQuotes(pt.get<std::string>("tag")));
  for (const auto& key : {"dockerapps", "target", "ostreehash"}) {
    headers.emplace_back("x-ats-" + std::string(key) + ": " + Utils::stripQuotes(pt.get<std::string>(key, "")));
  }
  auto http_client = std::make_shared<HttpClientWithShare>(&headers);

#ifdef BUILD_P11
  P11EngineGuard p11(config.p11.module, config.p11.pass, config.p11.label);
  std::string tls_ca = config.tls.ca_source == CryptoSource::kFile ? readFileIfExists(config.import.tls_cacert_path)
                                                                   : p11->getItemFullId(config.p11.tls_cacert_id);
  std::string tls_cert = config.tls.cert_source == CryptoSource::kFile
                             ? readFileIfExists(config.import.tls_clientcert_path)
                             : p11->getItemFullId(config.p11.tls_clientcert_id);
  std::string tls_pkey = config.tls.cert_source == CryptoSource::kFile ? readFileIfExists(config.import.tls_pkey_path)
                                                                       : p11->getItemFullId(config.p11.tls_pkey_id);
#else
  std::string tls_ca = readFileIfExists(config.import.tls_cacert_path);
  std::string tls_cert = readFileIfExists(config.import.tls_clientcert_path);
  std::string tls_pkey = readFileIfExists(config.import.tls_pkey_path);
#endif

  http_client->setCerts(tls_ca, config.tls.ca_source, tls_cert, config.tls.cert_source, tls_pkey,
                        config.tls.pkey_source);

  meta_fetcher_ = std::make_shared<Uptane::Fetcher>(config, http_client);
}

void AkHttpsRepoSource::fillConfig(Config& config, boost::property_tree::ptree& pt) {
  bool enable_hsm = pt.count("p11_module") > 0;
  if (!built_with_p11 && enable_hsm) {
    throw std::runtime_error("Aktualizr was built without PKCS#11 support, can't use \"p11_module\"");
  }

  if (enable_hsm) {
    config.p11.module = Utils::stripQuotes(pt.get<std::string>("p11_module"));
    config.p11.pass = Utils::stripQuotes(pt.get<std::string>("p11_pass"));
    config.p11.label = Utils::stripQuotes(pt.get<std::string>("p11_label"));
  }

  if (enable_hsm && pt.count("tls_pkey_id") > 0) {
    config.tls.pkey_source = CryptoSource::kPkcs11;
    config.p11.tls_pkey_id = Utils::stripQuotes(pt.get<std::string>("tls_pkey_id"));
  } else {
    config.tls.pkey_source = CryptoSource::kFile;
    config.import.tls_pkey_path = utils::BasedPath(Utils::stripQuotes(pt.get<std::string>("tls_pkey_path")));
  }

  if (enable_hsm && pt.count("tls_cacert_id") > 0) {
    config.tls.ca_source = CryptoSource::kPkcs11;
    config.p11.tls_cacert_id = Utils::stripQuotes(pt.get<std::string>("tls_cacert_id"));
  } else {
    config.tls.ca_source = CryptoSource::kFile;
    config.import.tls_cacert_path = utils::BasedPath(Utils::stripQuotes(pt.get<std::string>("tls_cacert_path")));
  }

  if (enable_hsm && pt.count("tls_clientcert_id") > 0) {
    config.tls.cert_source = CryptoSource::kPkcs11;
    config.p11.tls_clientcert_id = Utils::stripQuotes(pt.get<std::string>("tls_clientcert_id"));
  } else {
    config.tls.cert_source = CryptoSource::kFile;
    config.import.tls_clientcert_path =
        utils::BasedPath(Utils::stripQuotes(pt.get<std::string>("tls_clientcert_path")));
  }

  config.uptane.repo_server = Utils::stripQuotes(pt.get<std::string>("uri"));
}

std::string AkHttpsRepoSource::fetchRole(const Uptane::Role& role, int64_t maxsize, Uptane::Version version) {
  std::string reply;
  meta_fetcher_->fetchRole(&reply, maxsize, Uptane::RepositoryType::Image(), role, version);
  return reply;
}

std::string AkHttpsRepoSource::FetchRoot(int version) {
  return fetchRole(Uptane::Role::Root(), Uptane::kMaxRootSize, Uptane::Version(version));
}

std::string AkHttpsRepoSource::FetchTimestamp() {
  return fetchRole(Uptane::Role::Timestamp(), Uptane::kMaxTimestampSize, Uptane::Version());
}

std::string AkHttpsRepoSource::FetchSnapshot() {
  return fetchRole(Uptane::Role::Snapshot(), Uptane::kMaxSnapshotSize, Uptane::Version());
}

std::string AkHttpsRepoSource::FetchTargets() {
  return fetchRole(Uptane::Role::Targets(), Uptane::kMaxImageTargetsSize, Uptane::Version());
}

}  // namespace aklite::tuf
