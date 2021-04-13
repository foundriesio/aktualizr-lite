/**
* The SoftHSM token
*
* 1) Creates the softhsm2 configuration file
* 2) Initializes the softhsm token
*
*/
class SoftHsm {
 public:
  SoftHsm(std::string path, std::string conf)
      : label_("aktualizr"),
        pin_("87654321"),
        path_(std::move(path)),
        module_("/usr/lib/softhsm/libsofthsm2.so"),
        sopin_("12345678"),
        conf_(path_ + conf) {
    /* prepare softhsm2 work area */
    std::ofstream cfgOut(conf_);
    cfgOut << "directories.tokendir = " << path_ << std::endl;
    cfgOut << "log.level = DEBUG\n";
    cfgOut << "slots.removable = false\n";
    cfgOut.close();

    boost::format initToken("SOFTHSM2_CONF=%s softhsm2-util --init-token --free --label %s --so-pin %s --pin %s");
    cmd = boost::str(initToken % conf_ % label_ % sopin_ % pin_);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }

    /* NOTICE: system level environment configuration: must be set for libcurl */
    setenv("SOFTHSM2_CONF", conf_.c_str(), 1);

    LOG_INFO << "SoftHSM initialized";
  };

 public:
  std::string label_;
  std::string pin_;
  std::string path_;
  std::string module_;
  std::string conf_;

 private:
  std::string sopin_;
  /* buffers */
  std::string cmd;
  std::string out;
};


