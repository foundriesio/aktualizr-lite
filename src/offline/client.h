#ifndef AKTUALIZR_LITE_OFFLINE_CLIENT_H_
#define AKTUALIZR_LITE_OFFLINE_CLIENT_H_

#include <boost/filesystem.hpp>
#include <sstream>
#include <unordered_map>

#include "libaktualizr/config.h"
#include "uptane/fetcher.h"

#include "docker/dockerclient.h"
#include "liteclient.h"

namespace offline {

struct UpdateSrc {
  boost::filesystem::path TufDir;
  boost::filesystem::path OstreeRepoDir;
  boost::filesystem::path AppsDir;
  std::string TargetName;
};

enum class PostInstallAction { Undefined = -1, NeedReboot, NeedRebootForBootFw, NeedDockerRestart, AlreadyInstalled };
enum class PostRunAction {
  Undefined = -1,
  Ok,
  OkNeedReboot,
  RollbackOk,
  RollbackNeedReboot,
  RollbackToUnknown,
  RollbackToUnknownIfAppFailed
};

class MetaFetcher : public Uptane::IMetadataFetcher {
 public:
  class NotFoundException : public std::runtime_error {
   public:
    NotFoundException(const std::string& role, const std::string& version)
        : std::runtime_error("Metadata hasn't been found; role: " + role + "; version: " + version) {}
  };

 public:
  MetaFetcher(boost::filesystem::path tuf_repo_path, Uptane::Version max_root_ver = Uptane::Version())
      : tuf_repo_path_{std::move(tuf_repo_path)}, max_root_ver_{std::move(max_root_ver)} {}

  void fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo, const Uptane::Role& role,
                 Uptane::Version version) const override {
    const boost::filesystem::path meta_file_path{tuf_repo_path_ / version.RoleFileName(role)};

    if (!boost::filesystem::exists(meta_file_path) ||
        (role == Uptane::Role::Root() && max_root_ver_ != Uptane::Version() && max_root_ver_ < version)) {
      std::stringstream ver_str;
      ver_str << version;
      throw NotFoundException(role.ToString(), ver_str.str());
    }

    std::ifstream meta_file_stream(meta_file_path.string());
    *result = {std::istreambuf_iterator<char>(meta_file_stream), std::istreambuf_iterator<char>()};
  }

  void fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                       const Uptane::Role& role) const override {
    fetchRole(result, maxsize, repo, role, Uptane::Version());
  }

 private:
  const boost::filesystem::path tuf_repo_path_;
  const Uptane::Version max_root_ver_;
};

namespace client {

PostInstallAction install(const Config& cfg_in, const UpdateSrc& src,
                          std::shared_ptr<HttpInterface> docker_client_http_client = nullptr);
PostRunAction run(const Config& cfg_in, std::shared_ptr<HttpInterface> docker_client_http_client = nullptr);
const Uptane::Target getCurrent(const Config& cfg_in,
                                std::shared_ptr<HttpInterface> docker_client_http_client = nullptr);

}  // namespace client
}  // namespace offline

#endif  // AKTUALIZR_LITE_OFFLINE_CLIENT_H_
