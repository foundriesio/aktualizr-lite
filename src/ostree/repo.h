#ifndef AKTUALIZR_LITE_REPO_H
#define AKTUALIZR_LITE_REPO_H

#include <memory>
#include <string>
#include <unordered_map>

#include <ostree.h>

namespace OSTree {

class Repo {
 public:
  explicit Repo(std::string path, bool create = false);
  ~Repo();

  Repo(const Repo&) = delete;
  Repo(Repo&&) = delete;
  Repo& operator=(const Repo&) = delete;
  Repo& operator=(Repo&&) = delete;

  void addRemote(const std::string& name, const std::string& url, const std::string& ca, const std::string& cert,
                 const std::string& key);

  void pull(const std::string& remote_name, const std::string& branch, const std::string& commit_hash);
  void checkout(const std::string& commit_hash, const std::string& src_dir, const std::string& dst_dir);
  std::unordered_map<std::string, std::string> getRefs() const;

 private:
  void init(bool create);

  const std::string path_;
  OstreeRepo* repo_;
};

}  // namespace OSTree

#endif  // AKTUALIZR_LITE_REPO_H
