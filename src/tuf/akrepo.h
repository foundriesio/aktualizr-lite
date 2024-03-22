#include "libaktualizr/config.h"
#include "storage/invstorage.h"
#include "uptane/fetcher.h"
#include "uptane/imagerepository.h"

#include "aktualizr-lite/tuf/tuf.h"
#include "target.h"

namespace aklite::tuf {

// Repo implementation that uses libaktualizr to provide TUF metadata handling and storage
class AkRepo : public Repo {
 public:
  explicit AkRepo(const boost::filesystem::path& storage_path);
  explicit AkRepo(const Config& config);
  std::vector<TufTarget> GetTargets() override;
  std::string GetRoot(int version) override;
  void UpdateMeta(std::shared_ptr<RepoSource> repo_src) override;
  void CheckMeta() override;

 private:
  void init(const boost::filesystem::path& storage_path);

  Uptane::ImageRepository image_repo_;
  std::shared_ptr<INvStorage> storage_;

  // Wrapper around any TufRepoSource implementation to make it usable directly by libaktualizr,
  // by implementing Uptane::IMetadataFetcher interface
  class FetcherWrapper : public Uptane::IMetadataFetcher {
   public:
    explicit FetcherWrapper(std::shared_ptr<RepoSource> src);
    void fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo, const Uptane::Role& role,
                   Uptane::Version version) const override;

    void fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                         const Uptane::Role& role) const override;

   private:
    std::shared_ptr<RepoSource> repo_src;
  };
};

}  // namespace aklite::tuf
