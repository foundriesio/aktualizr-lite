#ifndef AKTUALIZR_LITE_COMPOSE_APP_TREE_H
#define AKTUALIZR_LITE_COMPOSE_APP_TREE_H

#include <string>

#include "ostree/repo.h"

class KeyManager;

class ComposeAppTree {
 public:
  static constexpr const char* const RemoteDefName{"treehub"};
  static constexpr const char* const ImagesDir{"/images"};
  static constexpr const char* const AppsDir{"/apps"};
  static constexpr const char* const Whiteouts{"/.whiteouts"};
  using Uri = std::tuple<std::string, std::string>;

 public:
  ComposeAppTree(const std::string& tree_path, std::string apps_dir, std::string images_dir, bool create = false);

 public:
  void pull(const std::string& remote_url, const KeyManager& key_manager, const std::string& uri);
  void checkout(const std::string& uri_str);

 private:
  void addRemote(const std::string& tree_remote, const KeyManager& key_manager);
  void applyWhiteouts(const std::string& hash);
  static Uri parseUri(const std::string& uri);

 private:
  OSTree::Repo repo_;
  const std::string apps_dir_;
  const std::string images_dir_;
  const std::string whiteouts_filepath_;
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_TREE_H
