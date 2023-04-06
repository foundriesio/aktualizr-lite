class SysRootFS {
 public:
  static std::string CreateCmd;

 public:
  SysRootFS(std::string _path, std::string _branch, std::string _hw_id, std::string _os)
      : branch{std::move(_branch)}, hw_id{std::move(_hw_id)}, path{std::move(_path)}, os{std::move(_os)}
  {
    executeCmd(CreateCmd, { path, branch, hw_id, os }, "generate a system rootfs template");
    // add bootloader version file
    Utils::writeFile(path + bootloader::BootloaderLite::VersionFile, std::string("bootfirmware_version=1"), true);
  }

  const std::string branch;
  const std::string hw_id;
  const std::string path;
  const std::string os;
};

std::string SysRootFS::CreateCmd;
