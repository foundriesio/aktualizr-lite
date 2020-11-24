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

 public:
  ComposeAppTree(const std::string& tree_path, std::string apps_dir, std::string images_dir, bool create = false);

 public:
  void pull(const std::string& remote_url, const KeyManager& key_manager, const std::string& hash);
  void checkout(const std::string& hash);

 private:
  void addRemote(const std::string& tree_remote, const KeyManager& key_manager);

 private:
  OSTree::Repo repo_;
  const std::string apps_dir_;
  const std::string images_dir_;
};

#endif  // AKTUALIZR_LITE_COMPOSE_APP_TREE_H
