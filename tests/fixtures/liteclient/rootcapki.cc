/**
* The RootCA
*
* 1) Generates the RootCA private key and certificate
* 2) Signs CSRs (using its private key and certificate)
*
*/
class RootCaPKI {
 public:
  RootCaPKI(std::string path, std::string key, std::string crt)
      : key_(path + std::move(key)), crt_(path + std::move(crt)) {
    try {
      boost::format generatePrivateKey("openssl ecparam -name prime256v1 -genkey -noout -out %s");
      cmd = boost::str(generatePrivateKey % key_);
      if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
        throw std::runtime_error(cmd.c_str());
      }

      boost::format generateCrt(
          "openssl req -new -key %s -subj \"/C=SP/ST=MALAGA/CN=ROOTCA\" -x509 -days 1000 -out %s");
      cmd = boost::str(generateCrt % key_ % crt_);
      if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
        throw std::runtime_error(cmd.c_str());
      }
    } catch (...) {
      LOG_INFO << "Cant create CA";
    }
  }

  void signCsr(std::string csr, std::string crt, std::string extra) {
    boost::format doSign("openssl x509 -req -days 1000 -sha256 %s -in %s -CA %s -CAkey %s -CAcreateserial -out %s");
    cmd = boost::str(doSign % extra % csr % crt_ % key_ % crt);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }
  }

 private:
  std::string key_;
  std::string crt_;
  /* buffers */
  std::string cmd;
  std::string out;
};


