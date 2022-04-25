class SysOSTreeRepoMock {
 public:
  SysOSTreeRepoMock(std::string _path, std::string _os) : path_{_path}, os_{_os}, repo_{path_ + "/ostree/repo"}
  {
    boost::filesystem::create_directories(path_);
    executeCmd("ostree", { "admin", "init-fs", path_ }, "init a system rootfs at " + path_);
    executeCmd("ostree", { "admin", "--sysroot=" + path_, "os-init", os_ }, "init OS in a system rootfs at " + path_);
    repo_.set_mode("bare-user-only");
    LOG_INFO << "System ostree-based repo has been initialized at " << path_;
  }

  const std::string& getPath() const { return path_; }
  OSTreeRepoMock& getRepo() { return repo_; }

  void deploy(const std::string& hash) {
    executeCmd("ostree", { "admin", "--sysroot=" + path_, "deploy", "--os=" + os_, hash }, "deploy " + hash);
  }

  void setMinFreeSpace(const std::string& size) {
    executeCmd("ostree", { "--repo=" + path_ + "/ostree/repo", "config", "set", "core.min-free-space-size", size }, "set config " + size);
  }

 private:
  const std::string path_;
  const std::string os_;
  OSTreeRepoMock repo_;
};


