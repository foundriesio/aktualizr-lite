#ifndef AKTUALIZR_LITE_AK_HTTP_REPO_SOURCE_H_
#define AKTUALIZR_LITE_AK_HTTP_REPO_SOURCE_H_

#include "uptane/fetcher.h"

#include "aktualizr-lite/tuf/tuf.h"

namespace aklite::tuf {

// TufRepoSource implementation for fetching remote meta-data via https using libaktualizr
class AkHttpsRepoSource : public RepoSource {
 public:
  AkHttpsRepoSource(const std::string& name_in, boost::property_tree::ptree& pt);
  AkHttpsRepoSource(const std::string& name_in, boost::property_tree::ptree& pt, Config& config);

  std::string FetchRoot(int version) override;
  std::string FetchTimestamp() override;
  std::string FetchSnapshot() override;
  std::string FetchTargets() override;

 private:
  void init(const std::string& name_in, boost::property_tree::ptree& pt, Config& config);
  static void fillConfig(Config& config, boost::property_tree::ptree& pt);
  std::string fetchRole(const Uptane::Role& role, int64_t maxsize, Uptane::Version version);

  std::string name_;
  std::shared_ptr<Uptane::IMetadataFetcher> meta_fetcher_;
};

}  // namespace aklite::tuf
#endif
