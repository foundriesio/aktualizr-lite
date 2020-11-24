#include "composeapptree.h"

#include "crypto/keymanager.h"

ComposeAppTree::ComposeAppTree(const std::string& tree_path, std::string apps_dir, std::string images_dir, bool create)
    : repo_{tree_path, create}, apps_dir_{std::move(apps_dir)}, images_dir_{std::move(images_dir)} {}

void ComposeAppTree::pull(const std::string& remote_url, const KeyManager& key_manager, const std::string& hash) {
  addRemote(remote_url, key_manager);
  repo_.pull(RemoteDefName, hash);
}

void ComposeAppTree::checkout(const std::string& hash) {
  repo_.checkout(hash, AppsDir, apps_dir_);
  repo_.checkout(hash, ImagesDir, images_dir_);
}

void ComposeAppTree::addRemote(const std::string& tree_remote, const KeyManager& key_manager) {
  repo_.addRemote(RemoteDefName, tree_remote, key_manager.getCaFile(), key_manager.getCertFile(),
                  key_manager.getPkeyFile());
}
