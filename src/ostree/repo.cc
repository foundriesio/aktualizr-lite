#include "repo.h"

#include <functional>
#include <iostream>
#include <stdexcept>

#include <ostree.h>
#include <boost/filesystem.hpp>
#include <boost/type_index.hpp>

namespace OSTree {

// The default value builtin in the libostree source code (see reload_core_config() function)
const unsigned int Repo::MinFreeSpacePercentDefaultValue{3};

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
  // NOLINTNEXTLINE(cppcoreguidelines-pro-type-cstyle-cast)
  repo_ = reinterpret_cast<OstreeRepo*>(g_steal_pointer(&repo));
}

void Repo::addRemote(const std::string& name, const std::string& url, const std::string& ca, const std::string& cert,
                     const std::string& key) {
  g_autoptr(GError) error = nullptr;
  GVariantBuilder var_builder;
  g_autoptr(GVariant) remote_options = nullptr;
  g_variant_builder_init(&var_builder, G_VARIANT_TYPE("a{sv}"));

  {
    g_variant_builder_add(&var_builder, "{s@v}", "gpg-verify", g_variant_new_variant(g_variant_new_boolean(FALSE)));

    if (!ca.empty()) {
      g_variant_builder_add(&var_builder, "{s@v}", "tls-ca-path",
                            g_variant_new_variant(g_variant_new_string(ca.c_str())));
      g_variant_builder_add(&var_builder, "{s@v}", "tls-client-cert-path",
                            g_variant_new_variant(g_variant_new_string(cert.c_str())));
      g_variant_builder_add(&var_builder, "{s@v}", "tls-client-key-path",
                            g_variant_new_variant(g_variant_new_string(key.c_str())));
    }
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

static void addRefInfo(gpointer key, gpointer value, gpointer user_data) {
  auto* const refs = reinterpret_cast<std::unordered_map<std::string, std::string>*>(user_data);
  refs->emplace(
      std::pair<std::string, std::string>{reinterpret_cast<const char*>(key), reinterpret_cast<const char*>(value)});
}

std::unordered_map<std::string, std::string> Repo::getRefs() const {
  g_autoptr(GHashTable) refs = nullptr;
  g_autoptr(GError) error = nullptr;

  if (0 == ostree_repo_list_refs(repo_, nullptr, &refs, nullptr, &error)) {
    throw std::runtime_error("Failed to list repo refs: " + std::string(error->message));
  }

  std::unordered_map<std::string, std::string> found_refs;

  g_hash_table_foreach(refs, addRefInfo, &found_refs);
  return found_refs;
}

std::string Repo::readFile(const std::string& commit_hash, const std::string& file) const {
  g_autoptr(GError) error = nullptr;
  g_autoptr(GFile) root = nullptr;
  GCancellable* cancellable = nullptr;

  if (0 == ostree_repo_read_commit(repo_, commit_hash.c_str(), &root, nullptr, nullptr, &error)) {
    throw std::runtime_error("Failed to read commit; commit: " + commit_hash + ", err: " + std::string(error->message));
  }

  g_autoptr(GFile) f = g_file_resolve_relative_path(root, file.c_str());
  g_autoptr(GInputStream) in = reinterpret_cast<GInputStream*>(g_file_read(f, cancellable, &error));
  if (in == nullptr) {
    throw std::runtime_error("Failed to open file; commit: " + commit_hash + ", file: " + file +
                             ", err: " + std::string(error->message));
  }

  std::size_t read_res;
  std::array<char, 1024> buffer{0};
  std::string file_content;
  while ((read_res = g_input_stream_read(in, buffer.data(), sizeof(buffer), nullptr, &error)) > 0) {
    file_content.append({buffer.data(), read_res});
  }
  if (read_res == -1) {
    throw std::runtime_error("Failed to read file from commit; commit: " + commit_hash + ", file: " + file +
                             ", err: " + std::string(error->message));
  }

  return file_content;
}

void Repo::setFreeSpacePercent(unsigned int min_free_space, bool hot_reload) {
  g_autoptr(GError) error = nullptr;
  GCancellable* cancellable = nullptr;

  g_autoptr(GKeyFile) config = ostree_repo_copy_config(repo_);
  g_key_file_set_string(config, "core", "min-free-space-percent", std::to_string(min_free_space).c_str());
  if (0 == ostree_repo_write_config(repo_, config, &error)) {
    throw std::runtime_error("Failed to set `min-free-space-percent`; value: " + std::to_string(min_free_space) +
                             ", err: " + std::string(error->message));
  }
  if (hot_reload) {
    if (0 == ostree_repo_reload_config(repo_, cancellable, &error)) {
      throw std::runtime_error("Failed to reload ostree repo config; repo path: " + path_ +
                               ", err: " + std::string(error->message));
    }
  }
}

unsigned int Repo::getFreeSpacePercent() const {
  g_autoptr(GError) error = nullptr;
  g_autofree char* low_watermark_str_val = nullptr;

  try {
    GKeyFile* readonly_config = ostree_repo_get_config(repo_);
    if (0 == g_key_file_has_key(readonly_config, "core", "min-free-space-percent", nullptr)) {
      return MinFreeSpacePercentDefaultValue;
    }
    low_watermark_str_val = g_key_file_get_string(readonly_config, "core", "min-free-space-percent", nullptr);
    const auto low_watermark_val{std::stoi(low_watermark_str_val)};
    return low_watermark_val;
  } catch (const std::exception& exc) {
    std::cerr << "Failed to get `min-free-space-percent` value: " << exc.what();
    return MinFreeSpacePercentDefaultValue;
  }
}

}  // namespace OSTree
