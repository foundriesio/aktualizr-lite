#include "repo.h"

#include <stdexcept>
#include <ostree.h>
#include <iostream>
#include <functional>
#include <boost/type_index.hpp>


namespace OSTree {

#define OSTREE_GIO_FAST_QUERYINFO ("standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target," \
                                   "unix::device,unix::inode,unix::mode,unix::uid,unix::gid,unix::rdev")



template<class Func, class ...Args>
void libostree_call(Func func, Args... args) {
  g_autoptr(GError) error = nullptr;
  //std::cout << FuncName << std::endl;
  if (!func(args..., &error)) {
    throw std::runtime_error("FuncName");
  }
}


Repo::Repo(std::string path): path_{std::move(path)}, repo_{nullptr} {
  init();
}

Repo::~Repo() {
  g_clear_object (&repo_);
}

void Repo::init() {
  OstreeRepoMode mode = OSTREE_REPO_MODE_BARE_USER;
  g_autoptr (GFile) path = nullptr;
  g_autoptr(OstreeRepo) repo = nullptr;
  g_autoptr(GError) error = nullptr;

  path = g_file_new_for_path (path_.c_str());
  // create OstreeRepo instance, not initialized nor bound to a specific repo on a file system
  repo = ostree_repo_new (path);

  // initialize OstreeRepo instance from a specified repo on a file system, if exists, if not
  // create it (repo file structure) and initialize OstreeRepo instance
//  if (!ostree_repo_create (repo, mode, nullptr, &error)) {
//    throw std::runtime_error("Failed to create or init an ostree repo at `" + path_ + "`: " + error->message);
//  }

  libostree_call(ostree_repo_create, repo, mode, nullptr);

  g_assert (repo_ == nullptr);
  repo_ = reinterpret_cast<OstreeRepo*>g_steal_pointer (&repo);
}

void Repo::addRemote(const std::string& name, const std::string& url,
                     const std::string& ca, const std::string& cert, const std::string& key) {

  g_autoptr(GError) error = nullptr;
  GVariantBuilder var_builder;
  g_autoptr(GVariant) remote_options = nullptr;
  g_variant_builder_init(&var_builder, G_VARIANT_TYPE("a{sv}"));

  {
    g_variant_builder_add(&var_builder, "{s@v}", "gpg-verify", g_variant_new_variant(g_variant_new_boolean(FALSE)));
    g_variant_builder_add(&var_builder, "{s@v}", "tls-ca-path", g_variant_new_variant(g_variant_new_string(ca.c_str())));
    g_variant_builder_add(&var_builder, "{s@v}", "tls-client-cert-path", g_variant_new_variant(g_variant_new_string(cert.c_str())));
    g_variant_builder_add(&var_builder, "{s@v}", "tls-client-key-path", g_variant_new_variant(g_variant_new_string(key.c_str())));
  }

  remote_options = g_variant_builder_end(&var_builder);
//  if (!ostree_repo_remote_change(repo_, nullptr, OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS,
//                                 name.c_str(), url.c_str(), remote_options, nullptr, &error)) {
//    throw std::runtime_error("Failed to add a remote to " + path_ + ": " + error->message);
//  }
  libostree_call(ostree_repo_remote_change, repo_, nullptr, OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS,
                                           name.c_str(), url.c_str(), remote_options, nullptr);
}

void Repo::pull(const std::string& remote_name, const std::string& hash) {
  OstreeAsyncProgress* progress = nullptr;
  OstreeRepoPullFlags pull_flags = OSTREE_REPO_PULL_FLAGS_NONE;
  char* ref_to_fetch[] = {const_cast<char*>(hash.c_str()), NULL};

  g_autoptr(GError) error = nullptr;
  gboolean pull_result = ostree_repo_pull(repo_, remote_name.c_str(),
                                          (char**)&ref_to_fetch, pull_flags,
                                          progress, nullptr, &error);

  if (!pull_result) {
    throw std::runtime_error("Failed to pull " + hash + ": " + error->message);
  }
}


void Repo::checkout(const std::string& hash, const std::string& src_dir, const std::string& dst_dir) {

  OstreeRepoCheckoutMode checkout_mode = OSTREE_REPO_CHECKOUT_MODE_USER;
  OstreeRepoCheckoutOverwriteMode overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  g_autoptr(GFile) root = nullptr;
  g_autoptr(GFile) src = nullptr;
  g_autoptr(GFile) dst = nullptr;
  g_autoptr(GFileInfo) file_info = NULL;


  {
    g_autoptr(GError) error = nullptr;
    if (!ostree_repo_read_commit (repo_, hash.c_str(), &root, NULL, nullptr, &error)) {
      throw std::runtime_error("Failed to read commit " + hash + ": " + error->message);
    }
  }

  src = g_file_resolve_relative_path (root, src_dir.c_str());

  {
    g_autoptr(GError) error = nullptr;
    file_info = g_file_query_info (src, OSTREE_GIO_FAST_QUERYINFO,
                                  G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS,
                                  nullptr, &error);
    if (!file_info) {
      throw std::runtime_error("Failed to query file info " + src_dir + ": " + error->message);
    }
  }

  dst = g_file_new_for_path (dst_dir.c_str());
  {
    g_autoptr(GError) error = nullptr;
    if (!ostree_repo_checkout_tree(repo_, checkout_mode, overwrite_mode, dst,
                                   OSTREE_REPO_FILE (src), file_info, nullptr, &error)) {

      throw std::runtime_error("Failed to checkout tree form repo " + hash + ": " + error->message);
    }
  }

}

} // namespace OSTree
