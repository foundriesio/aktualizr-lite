#include "akrepo.h"
#include "target.h"

namespace aklite::tuf {

// AkRepo
AkRepo::AkRepo(const boost::filesystem::path& storage_path) { init(storage_path); }

AkRepo::AkRepo(const Config& config) {
  storage_ = INvStorage::newStorage(config.storage, false, StorageClient::kTUF);
  storage_->importData(config.import);
}

std::vector<TufTarget> AkRepo::GetTargets() {
  std::shared_ptr<const Uptane::Targets> targets{image_repo_.getTargets()};
  if (targets) {
    auto ret = std::vector<TufTarget>();
    for (const auto& up_target : image_repo_.getTargets()->targets) {
      ret.emplace_back(Target::toTufTarget(up_target));
    }
    return ret;
  } else {
    return std::vector<TufTarget>();
  }
}

void AkRepo::updateMeta(std::shared_ptr<RepoSource> repo_src) {
  FetcherWrapper wrapper(repo_src);
  image_repo_.updateMeta(*storage_, wrapper);
}

void AkRepo::init(const boost::filesystem::path& storage_path) {
  StorageConfig sc;
  sc.path = storage_path;
  storage_ = INvStorage::newStorage(sc, false, StorageClient::kTUF);
}

// FetcherWrapper
AkRepo::FetcherWrapper::FetcherWrapper(std::shared_ptr<RepoSource> src) { repo_src = std::move(src); }

void AkRepo::FetcherWrapper::fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                                       const Uptane::Role& role, Uptane::Version version) const {
  (void)maxsize;
  (void)repo;
  std::string json;
  if (role == Uptane::Role::Root()) {
    json = repo_src->fetchRoot(version.version());
  } else if (role == Uptane::Role::Timestamp()) {
    json = repo_src->fetchTimestamp();
  } else if (role == Uptane::Role::Snapshot()) {
    json = repo_src->fetchSnapshot();
  } else if (role == Uptane::Role::Targets()) {
    json = repo_src->fetchTargets();
  } else {
    throw std::runtime_error("Invalid TUF Role " + role.ToString());
  }
  *result = json;
}

void AkRepo::FetcherWrapper::fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                                             const Uptane::Role& role) const {
  fetchRole(result, maxsize, repo, role, Uptane::Version());
}

}  // namespace aklite::tuf
