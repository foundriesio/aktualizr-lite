/**
* The PKI file server
*
* 1) Generates its private key and a CSR
* 2) Requests its CRT from the RootCA
*
*/
class ServerPKI {
 public:
  ServerPKI(std::string path, RootCaPKI& rootCa, std::string csr, std::string crt, std::string key) {
    /* hardcoded names as required by the http server */
    csr = path + csr;
    crt = path + crt;
    key = path + key;
    std::string xtr = path + "/altname.txt";

    boost::format generatePrivateKey("openssl genrsa -out %s 2048");
    cmd = boost::str(generatePrivateKey % key);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }

    boost::format generateCsr("openssl req -new -sha256 -key %s -subj \"/C=SP/ST=MALAGA/CN=localhost\" -out %s");
    cmd = boost::str(generateCsr % key % csr);
    if (Utils::shell(cmd, &out, true) != EXIT_SUCCESS) {
      LOG_INFO << "Error: " << out;
      throw std::runtime_error(cmd.c_str());
    }
    std::string info = "subjectAltName = DNS:localhost\n";
    Utils::writeFile(xtr, info, false);
    rootCa.signCsr(csr, crt, "-extfile " + xtr);
  }

 private:
  std::string cmd;
  std::string out;
};


