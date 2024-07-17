#ifndef AKTUALIZR_LITE_AK_LOCAL_REPO_SOURCE_H_
#define AKTUALIZR_LITE_AK_LOCAL_REPO_SOURCE_H_

#include "boost/filesystem.hpp"

#include "aktualizr-lite/tuf/tuf.h"

namespace aklite::tuf {

// TufRepoSource implementation for fetching local meta-dat
class LocalRepoSource : public RepoSource {
 public:
  LocalRepoSource(const std::string &name_in, const std::string &local_path);

  std::string FetchRoot(int version) override;
  std::string FetchTimestamp() override;
  std::string FetchSnapshot() override;
  std::string FetchTargets() override;

  std::string SourceDir();

 private:
  static std::string fetchFile(const boost::filesystem::path &meta_file_path);

  std::string name;
  boost::filesystem::path src_dir_;
};

}  // namespace aklite::tuf

#endif
