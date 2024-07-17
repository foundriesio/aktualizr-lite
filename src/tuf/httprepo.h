#ifndef AKTUALIZR_LITE_HTTP_REPO_H_
#define AKTUALIZR_LITE_HTTP_REPO_H_

#include "libaktualizr/config.h"
#include "storage/invstorage.h"
#include "uptane/fetcher.h"
#include "uptane/imagerepository.h"

#include "aktualizr-lite/tuf/tuf.h"
#include "target.h"

namespace aklite::tuf {

// Repo implementation that uses libaktualizr to provide TUF metadata handling and storage
class HttpRepo : public Repo {
 public:
  explicit HttpRepo(const boost::filesystem::path& storage_path);
  explicit HttpRepo(const Config& config);
  std::vector<TufTarget> GetTargets() override;
  std::string GetRoot(int version) override;
  void UpdateMeta(std::shared_ptr<RepoSource> repo_src) override;
  void CheckMeta() override;
};

}  // namespace aklite::tuf

#endif
