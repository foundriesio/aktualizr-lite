class FioVb {
 public:
  FioVb(const boost::filesystem::path& dir):dir_{dir} {
    const std::string path{dir.string()};
    setenv("PATH", (path + ":" + getenv("PATH")).c_str(), 1);
    const auto script_file{dir / "fiovb_setenv"};
    // Create `fiovb_setenv` mock dynamically.
    // It stores bootloader variable values in corresponding files. Then unit tests can read and check the variable values.
    Utils::writeFile(script_file, boost::str(boost::format(script_) % dir_));
    boost::filesystem::permissions(script_file, boost::filesystem::status(script_file).permissions() | boost::filesystem::perms::owner_exe);
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

 private:
  const boost::filesystem::path dir_;
  static const std::string script_;
};

const std::string FioVb::script_ = "#!/bin/bash\n\n echo ${2} > %s/${1}";
