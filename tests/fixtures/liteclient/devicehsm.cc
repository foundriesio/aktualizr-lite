/**
* The Device actions to register with the HSM token
*
*  1) Generates the ssl configuration file to allow using the HSM (softhsm)
*  2) Stores the device key in the HSM (public/private keypair)
*  3) Creates a CSR and requests its CRT from the RootCA
*  4) Imports the CRT into the HSM.
*  5) Generates HSM debug information if needed
*
*/
class DeviceHsm {
 public:
  DeviceHsm(SoftHsm* hsm, RootCaPKI& rootCa, std::string conf) : hsm_(hsm), rootCa_(rootCa), cnf_(hsm_->path_ + conf) {
    std::ofstream cnfOut(cnf_);
    cnfOut << "openssl_conf = oc\n";
    cnfOut << "[oc]\n";
    cnfOut << "engines = eng\n";
    cnfOut << "[eng]\n";
    cnfOut << "pkcs11 = p11\n";
    cnfOut << "[p11]\n";
    cnfOut << "engine_id = pkcs11\n";
    cnfOut << "dynamic_path = /usr/lib/x86_64-linux-gnu/engines-1.1/pkcs11.so\n";
    cnfOut << "MODULE_PATH = " << hsm_->module_ << std::endl;
    cnfOut << "init = 0\n";
    cnfOut << "PIN = " << hsm_->pin_ << std::endl;
    cnfOut << "[req]\n";
    cnfOut << "prompt = no\n";
    cnfOut << "distinguished_name = dn\n";
    cnfOut << "req_extensions = ext\n";
    cnfOut << "[dn]\n";
    cnfOut << "C = SP\n";
    cnfOut << "ST = MALAGA\n";
    cnfOut << "CN = DeviceHSM\n";
    cnfOut << "OU = Factory\n";
    cnfOut << "[ext]\n";
    cnfOut << "keyUsage = critical, digitalSignature\n";
    cnfOut << "extendedKeyUsage = critical, clientAuth\n";
    cnfOut.close();
  }

  void createKey(std::string id, std::string label) {
    boost::format generateKeyPair(
        "pkcs11-tool --module %s --keypairgen --key-type EC:prime256v1 --token-label %s --id %s "
        "--label %s --pin %s");
    cmd = boost::str(generateKeyPair % hsm_->module_ % hsm_->label_ % id % label % hsm_->pin_);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }
  }

  void createCsr(std::string label, std::string& csr) {
    boost::format keyFmt("\"pkcs11:token=%s;object=%s;type=private;pin-value=%s\"");
    std::string key = boost::str(keyFmt % hsm_->label_ % label % hsm_->pin_);

    boost::format doCsr("OPENSSL_CONF=%s openssl req -new -engine pkcs11 -keyform engine -key %s");
    cmd = boost::str(doCsr % cnf_ % key);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }

    /* write CSR to disk */
    csr = hsm_->path_ + csr;
    Utils::writeFile(csr, out, true);
  }

  void createCrt(std::string csr, std::string& crt) {
    crt = hsm_->path_ + crt;
    rootCa_.signCsr(csr, crt, "");
  }

  void importCrt(std::string& crt, std::string id) {
    boost::format crtToDer("OPENSSL_CONF=%s openssl x509 -inform pem -in %s -out %s/tmp.der");
    cmd = boost::str(crtToDer % cnf_ % crt % hsm_->path_);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }

    boost::format writeCrtToHsm("pkcs11-tool --module %s -w %s/tmp.der -y cert --id %s --pin %s");
    cmd = boost::str(writeCrtToHsm % hsm_->module_ % hsm_->path_ % id % hsm_->pin_);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }
  }

  void listInfo() {
    boost::format listMechanisms("pkcs11-tool --module %s --list-mechanisms");
    cmd = boost::str(listMechanisms % hsm_->module_);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }
    // very verbose: enable if debug needed
    // LOG_INFO << out;
    boost::format listObjects("pkcs11-tool --module %s --list-objects");
    cmd = boost::str(listObjects % hsm_->module_);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      throw std::runtime_error(cmd.c_str());
    }
    // very verbose: enable if debug needed
    // LOG_INFO << out;
  }

 private:
  SoftHsm* hsm_;
  RootCaPKI& rootCa_;
  std::string cnf_;
  /* buffers */
  std::string cmd;
  std::string out;
};

