#include "fetcher.h"


namespace offline {


void Fetcher::fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo, const Uptane::Role& role,
                         Uptane::Version version) const {
  LOG_ERROR << ">>>>>>>>>>>>>> Fetching ROLE: " << role.ToString() << "; version: " << version;
  LOG_ERROR << ">>>>>>>>>>>>>> Role filename: " << version.RoleFileName(role);

  const auto metadata_file{repo_dir_ / version.RoleFileName(role)};
  if (!boost::filesystem::exists(metadata_file)) {
    throw Uptane::MetadataFetchFailure(repo.ToString(), role.ToString());
  }

  *result = Utils::jsonToCanonicalStr(Utils::parseJSONFile(metadata_file));
}

void Fetcher::fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                               const Uptane::Role& role) const {
  LOG_ERROR << ">>>>>>>>>>>>>> Fetching Latest ROLE: " << role.ToString();
  LOG_ERROR << ">>>>>>>>>>>>>> Role filename: " << role.ToString();
  const auto metadata_file{repo_dir_ / (role.ToString() + ".json")};
  if (!boost::filesystem::exists(metadata_file)) {
    throw Uptane::MetadataFetchFailure(repo.ToString(), role.ToString());
  }
  *result = Utils::jsonToCanonicalStr(Utils::parseJSONFile(metadata_file));
}

} // namespace offline
