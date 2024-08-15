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
    throw MetadataNotFoundException(meta_file_path.string());
  }

  return Utils::readFile(meta_file_path);
}

std::string LocalRepoSource::FetchRoot(int version) {
  return fetchFile(src_dir_ / (std::to_string(version) + ".root.json"));
}

std::string LocalRepoSource::FetchTimestamp() { return fetchFile(src_dir_ / "timestamp.json"); }

std::string LocalRepoSource::FetchSnapshot() { return fetchFile(src_dir_ / "snapshot.json"); }

std::string LocalRepoSource::FetchTargets() { return fetchFile(src_dir_ / "targets.json"); }

std::string LocalRepoSource::SourceDir() { return src_dir_.string(); }

}  // namespace aklite::tuf
