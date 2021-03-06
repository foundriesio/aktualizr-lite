#include "composeapptree.h"

#include <vector>
#include "crypto/keymanager.h"

ComposeAppTree::ComposeAppTree(const std::string& tree_path, std::string apps_dir, std::string images_dir, bool create)
    : repo_{tree_path, create},
      apps_dir_{std::move(apps_dir)},
      images_dir_{std::move(images_dir)},
      whiteouts_filepath_{(boost::filesystem::path{images_dir_} / Whiteouts).string()} {}

void ComposeAppTree::pull(const std::string& remote_url, const KeyManager& key_manager, const std::string& uri) {
  addRemote(remote_url, key_manager);
  std::string branch;
  std::string commit_hash;
  std::tie(branch, commit_hash) = parseUri(uri);
  repo_.pull(RemoteDefName, branch, commit_hash);
}

void ComposeAppTree::checkout(const std::string& uri_str) {
  const auto uri = parseUri(uri_str);
  const auto& commit_hash = std::get<1>(uri);
  repo_.checkout(commit_hash, AppsDir, apps_dir_);
  repo_.checkout(commit_hash, ImagesDir, images_dir_);
  applyWhiteouts(commit_hash);
}

void ComposeAppTree::addRemote(const std::string& tree_remote, const KeyManager& key_manager) {
  repo_.addRemote(RemoteDefName, tree_remote, key_manager.getCaFile(), key_manager.getCertFile(),
                  key_manager.getPkeyFile());
}

void ComposeAppTree::applyWhiteouts(const std::string& hash) {
  repo_.checkout(hash, Whiteouts, images_dir_);

  LOG_DEBUG << "Processing the file containing non-regular file records: " << whiteouts_filepath_;
  std::ifstream whiteouts_file(whiteouts_filepath_);
  std::string line;

  while (std::getline(whiteouts_file, line)) {
    std::vector<std::string> result;
    boost::split(result, line, boost::is_any_of(" "));

    if (3 != result.size()) {
      LOG_ERROR << "Invalid the non-regular file record: expected three items got " << result.size();
      return;
    }

    auto dst_file = boost::filesystem::path(images_dir_) / result[0];
    mode_t dst_file_mode = std::stoi(result[1]);
    dev_t dst_file_device = std::stoi(result[2]);

    if (boost::filesystem::exists(dst_file)) {
      LOG_DEBUG << "A non-regular file has been already created: " << dst_file;
      continue;
    }

    LOG_DEBUG << "Creating a non-regular file; path: " << dst_file << " mode: " << dst_file_mode << " device "
              << dst_file_device;

    if (-1 == mknod(dst_file.c_str(), dst_file_mode, dst_file_device)) {
      LOG_ERROR << "Failed to create a non-regular file: " << dst_file << strerror(errno);
    }
  }
}

ComposeAppTree::Uri ComposeAppTree::parseUri(const std::string& uri) {
  const auto uri_separator_indx = uri.find_first_of('@');
  if (uri_separator_indx == std::string::npos) {
    throw std::invalid_argument("Invalid app uri: " + uri);
  }
  const auto branch{uri.substr(0, uri_separator_indx)};
  const auto commit_hash{uri.substr(uri_separator_indx + 1)};
  return ComposeAppTree::Uri{branch, commit_hash};
}
