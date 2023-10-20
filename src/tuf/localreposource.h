#include "boost/filesystem.hpp"

#include "aktualizr-lite/tuf/tuf.h"

namespace aklite::tuf {

// TufRepoSource implementation for fetching local meta-dat
class LocalRepoSource : public RepoSource {
 public:
  class NotFoundException : public std::runtime_error {
   public:
    explicit NotFoundException(const std::string &path)
        : std::runtime_error("Metadata hasn't been found; file path: " + path) {}
  };

  LocalRepoSource(const std::string &name_in, const std::string &local_path);

  std::string fetchRoot(int version) override;
  std::string fetchTimestamp() override;
  std::string fetchSnapshot() override;
  std::string fetchTargets() override;

 private:
  static std::string fetchFile(const boost::filesystem::path &meta_file_path);

  std::string name;
  boost::filesystem::path src_dir_;
};

}  // namespace aklite::tuf
