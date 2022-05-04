#ifndef AKTUALIZR_LITE_OFFLINE_FETCHER_H_
#define AKTUALIZR_LITE_OFFLINE_FETCHER_H_

#include "uptane/fetcher.h"

namespace offline {


class Fetcher: public Uptane::IMetadataFetcher {
 public:
    Fetcher(): IMetadataFetcher(), repo_dir_{"/work/fio/factories/msul-dev01/TUF/tuf-meta"} {}
    virtual ~Fetcher() {}

    void fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo, const Uptane::Role& role,
                         Uptane::Version version) const override;
    void fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                               const Uptane::Role& role) const override;

 private:

 private:
   boost::filesystem::path repo_dir_;
};


} // namespace offline


#endif // AKTUALIZR_LITE_OFFLINE_FETCHER_H_
