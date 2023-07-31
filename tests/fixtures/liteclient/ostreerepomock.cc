class OSTreeRepoMock {
 public:
  OSTreeRepoMock(std::string path, bool create = false, std::string mode = "archive") : path_{std::move(path)}
  {
    if (!create) return;
    executeCmd("ostree", { "init", "--repo", path_, "--mode=" + mode }, "init an ostree repo at " + path_);
  }

  void pullLocal(const std::string& src_dir, const std::string& hash) {
    executeCmd("ostree", { "pull-local", "--repo", path_, src_dir, hash },
                         "pulling " + hash + " from " + src_dir + " to " + path_);
  }


  std::string commit(const std::string& src_dir, const std::string& branch) {
    return executeCmd("ostree", { "commit", "--repo", path_, "--branch", branch, "--tree=dir=" + src_dir },
                      "commit from " + src_dir + " to " + path_);
  }

  void set_mode(const std::string& mode) {
    executeCmd("ostree", { "config", "--repo", path_, "set", "core.mode", mode }, "set mode for repo " + path_);
  }

  void generate_delta(const std::string& from, const std::string& to) {
    executeCmd("ostree", {"static-delta", "generate", "--repo", path_, "--from", from, "--to", to},
               "generate static delta between " + from + " and " + to);
    executeCmd("ostree", {"summary", "--repo", path_, "-u"}, "update summary with delta indexes");
  }

  const std::string& getPath() const { return path_; }

 private:
  const std::string path_;
};


