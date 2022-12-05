class BootFlagMgr {
 public:
  static const std::string SetScript;
  static const std::string GetScript;

  using Ptr = std::shared_ptr<BootFlagMgr>;

  BootFlagMgr(const boost::filesystem::path& dir, const std::vector<std::tuple<std::string, std::string>>& scripts):dir_{dir} {
    const std::string path{dir.string()};
    setenv("PATH", (path + ":" + getenv("PATH")).c_str(), 1);

    for (const auto s: scripts) {
      const std::string script{std::get<0>(s)};
      const std::string name{std::get<1>(s)};
      const auto script_file{dir / name};
      // Create the boot management script/mock dynamically.
      // It stores bootloader variable values in corresponding files. Then unit tests can read and check the variable values.
      Utils::writeFile(script_file, boost::str(boost::format(script) % dir_));
      boost::filesystem::permissions(script_file, boost::filesystem::status(script_file).permissions() | boost::filesystem::perms::owner_exe);
    }
  }

  int bootcount() const {
   return std::stoi(Utils::readFile(dir_/"bootcount"));
  }
  int upgrade_available() const {
   return std::stoi(Utils::readFile(dir_/"upgrade_available"));
  }
  int rollback() const {
   return std::stoi(Utils::readFile(dir_/"rollback"));
  }
  int bootupgrade_available() const {
   return std::stoi(Utils::readFile(dir_/"bootupgrade_available"));
  }

  void reset_bootupgrade_available() {
    Utils::writeFile(dir_/"bootupgrade_available", std::string("0"));
  }

 private:
  const boost::filesystem::path dir_;
};

const std::string BootFlagMgr::SetScript = "#!/bin/bash\n\n echo ${2} > %s/${1}";
const std::string BootFlagMgr::GetScript = "#!/bin/bash\n\n cat %s/${1}";

class FioVb: public BootFlagMgr {
  public:
    FioVb(const boost::filesystem::path& dir): BootFlagMgr(dir, {
      {SetScript, "fiovb_setenv"},
      {GetScript, "fiovb_printenv"},
    }){}
};


class UbootFlagMgr: public BootFlagMgr {
 public:
  static const std::string GetScript;

   UbootFlagMgr(const boost::filesystem::path& dir): BootFlagMgr(dir, {
      {BootFlagMgr::SetScript, "fw_setenv"},
      {UbootFlagMgr::GetScript, "fw_printenv"},
    }){}
};


const std::string UbootFlagMgr::GetScript = "#!/bin/bash\n\n cat %s/${2}";
