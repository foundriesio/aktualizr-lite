#include "repo.h"

#include <functional>
#include <iostream>
#include <stdexcept>

#include <ostree.h>
#include <boost/filesystem.hpp>
#include <boost/type_index.hpp>

namespace OSTree {

Repo::Repo(std::string path, bool create) : path_{std::move(path)}, repo_{nullptr} { init(create); }

Repo::~Repo() {
  // NOLINTNEXTLINE (bugprone-sizeof-expression)
  g_clear_object(&repo_);
}

void Repo::init(bool create) {
  OstreeRepoMode mode = OSTREE_REPO_MODE_BARE;
  g_autoptr(GFile) path = nullptr;
  g_autoptr(OstreeRepo) repo = nullptr;
  g_autoptr(GError) error = nullptr;

  path = g_file_new_for_path(path_.c_str());
  // create OstreeRepo instance, not initialized nor bound to a specific repo on a file system
  repo = ostree_repo_new(path);

  if (create) {
    // initialize OstreeRepo instance from a specified repo on a file system if it exists, if not it
    // create a repo file structure and initialize OstreeRepo instance
    if (0 == ostree_repo_create(repo, mode, nullptr, &error)) {
      throw std::runtime_error("Failed to create or init an ostree repo at `" + path_ + "`: " + error->message);
    }
  } else {
    if (0 == ostree_repo_open(repo, nullptr, &error)) {
      throw std::runtime_error("Failed to init an ostree repo at `" + path_ + "`: " + error->message);
    }
  }

  g_assert(repo_ == nullptr);
  repo_ = reinterpret_cast<OstreeRepo*> g_steal_pointer(&repo);
}

void Repo::addRemote(const std::string& name, const std::string& url, const std::string& ca, const std::string& cert,
                     const std::string& key) {
  g_autoptr(GError) error = nullptr;
  GVariantBuilder var_builder;
  g_autoptr(GVariant) remote_options = nullptr;
  g_variant_builder_init(&var_builder, G_VARIANT_TYPE("a{sv}"));

  {
    g_variant_builder_add(&var_builder, "{s@v}", "gpg-verify", g_variant_new_variant(g_variant_new_boolean(FALSE)));
    g_variant_builder_add(&var_builder, "{s@v}", "tls-ca-path",
                          g_variant_new_variant(g_variant_new_string(ca.c_str())));
    g_variant_builder_add(&var_builder, "{s@v}", "tls-client-cert-path",
                          g_variant_new_variant(g_variant_new_string(cert.c_str())));
    g_variant_builder_add(&var_builder, "{s@v}", "tls-client-key-path",
                          g_variant_new_variant(g_variant_new_string(key.c_str())));
  }

  remote_options = g_variant_builder_end(&var_builder);

  if (ostree_repo_remote_change(repo_, nullptr, OSTREE_REPO_REMOTE_CHANGE_DELETE_IF_EXISTS, name.c_str(), url.c_str(),
                                remote_options, nullptr, &error) == 0) {
    throw std::runtime_error("Failed to delete a current remote from " + path_ + ": " + error->message);
  }

  if (0 == ostree_repo_remote_change(repo_, nullptr, OSTREE_REPO_REMOTE_CHANGE_ADD_IF_NOT_EXISTS, name.c_str(),
                                     url.c_str(), remote_options, nullptr, &error)) {
    throw std::runtime_error("Failed to add a remote to " + path_ + ": " + error->message);
  }
}

void Repo::pull(const std::string& remote_name, const std::string& branch, const std::string& commit_hash) {
  OstreeAsyncProgress* progress =
      ostree_async_progress_new_and_connect(ostree_repo_pull_default_console_progress_changed, nullptr);
  OstreeRepoPullFlags pull_flags = OSTREE_REPO_PULL_FLAGS_NONE;
  std::array<char*, 2> ref_to_fetch{const_cast<char*>(branch.c_str()), nullptr};
  std::array<char*, 2> commit_id{const_cast<char*>(commit_hash.c_str()), nullptr};

  GVariantBuilder builder;
  g_autoptr(GVariant) pull_options = nullptr;
  g_autoptr(GError) error = nullptr;

  g_variant_builder_init(&builder, G_VARIANT_TYPE("a{sv}"));

  g_variant_builder_add(
      &builder, "{s@v}", "refs",
      g_variant_new_variant(g_variant_new_strv(reinterpret_cast<const char* const*>(&ref_to_fetch), -1)));

  g_variant_builder_add(
      &builder, "{s@v}", "override-commit-ids",
      g_variant_new_variant(g_variant_new_strv(reinterpret_cast<const char* const*>(&commit_id), -1)));

  pull_options = g_variant_ref_sink(g_variant_builder_end(&builder));

  gboolean pull_result =
      ostree_repo_pull_with_options(repo_, remote_name.c_str(), pull_options, progress, nullptr, &error);

  if (0 == pull_result) {
    throw std::runtime_error("Failed to pull " + branch + "@" + commit_hash + ": " + error->message);
  }
}

void Repo::checkout(const std::string& commit_hash, const std::string& src_dir, const std::string& dst_dir) {
  const char* const OSTREE_GIO_FAST_QUERYINFO =
      "standard::name,standard::type,standard::size,standard::is-symlink,standard::symlink-target,unix::device,unix::"
      "inode,unix::mode,unix::uid,unix::gid,unix::rdev";
  OstreeRepoCheckoutMode checkout_mode = OSTREE_REPO_CHECKOUT_MODE_NONE;
  OstreeRepoCheckoutOverwriteMode overwrite_mode = OSTREE_REPO_CHECKOUT_OVERWRITE_UNION_FILES;
  g_autoptr(GFile) root = nullptr;
  g_autoptr(GFile) src = nullptr;
  g_autoptr(GFile) dst = nullptr;
  g_autoptr(GFileInfo) file_info = nullptr;

  {
    g_autoptr(GError) error = nullptr;
    if (0 == ostree_repo_read_commit(repo_, commit_hash.c_str(), &root, nullptr, nullptr, &error)) {
      throw std::runtime_error("Failed to read commit " + commit_hash + ": " + error->message);
    }
  }

  src = g_file_resolve_relative_path(root, src_dir.c_str());

  {
    g_autoptr(GError) error = nullptr;
    file_info = g_file_query_info(src, OSTREE_GIO_FAST_QUERYINFO, G_FILE_QUERY_INFO_NOFOLLOW_SYMLINKS, nullptr, &error);
    if (nullptr == file_info) {
      throw std::runtime_error("Failed to query file info " + src_dir + ": " + error->message);
    }
  }

  dst = g_file_new_for_path(dst_dir.c_str());
  {
    g_autoptr(GError) error = nullptr;
    // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
    if (0 == ostree_repo_checkout_tree(repo_, checkout_mode, overwrite_mode, dst, OSTREE_REPO_FILE(src), file_info,
                                       nullptr, &error)) {
      throw std::runtime_error("Failed to checkout tree form repo " + commit_hash + ": " + error->message);
    }
  }
}

}  // namespace OSTree
