#include <boost/filesystem.hpp>

#include "utilities/utils.h"

#include "localreposource.h"

namespace aklite::tuf {

LocalRepoSource::LocalRepoSource(const std::string &name_in, const std::string &local_path) {
  name = name_in;
  src_dir_ = boost::filesystem::path(local_path);
}

std::string LocalRepoSource::fetchFile(const boost::filesystem::path &meta_file_path) {
  if (!boost::filesystem::exists(meta_file_path)) {
    throw NotFoundException(meta_file_path.string());
  }

  return Utils::readFile(meta_file_path);
}

// DISCUSS: limit to max version?
std::string LocalRepoSource::fetchRoot(int version) {
  return fetchFile(src_dir_ / (std::to_string(version) + ".root.json"));
}

std::string LocalRepoSource::fetchTimestamp() { return fetchFile(src_dir_ / "timestamp.json"); }

std::string LocalRepoSource::fetchSnapshot() { return fetchFile(src_dir_ / "snapshot.json"); }

std::string LocalRepoSource::fetchTargets() { return fetchFile(src_dir_ / "targets.json"); }

}  // namespace aklite::tuf
