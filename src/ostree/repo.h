#ifndef REPO_H
#define REPO_H

#include <string>
#include <memory>

#include <ostree.h>


namespace OSTree {

class Repo {
 public:
  using Ptr = std::shared_ptr<Repo>;

 public:
  static Ptr create(std::string path) { return std::make_shared<Repo>(std::move(path)); }

  Repo(std::string path);
  ~Repo();

 public:
  void addRemote(const std::string& url, const std::string& name,
                 const std::string& ca, const std::string& cert, const std::string& key);

  void pull(const std::string& remote_name, const std::string& hash);
  void checkout(const std::string& hash, const std::string& src_dir, const std::string& dst_dir);

 private:
  void init();

 private:
  const std::string path_;
  OstreeRepo* repo_;
};

} // namespace OSTree

#endif // REPO_H
