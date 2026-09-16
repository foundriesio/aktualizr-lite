#include <libp11.h>
#include <openssl/err.h>
#include <openssl/pem.h>
#include <openssl/x509v3.h>

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
    // Signed in-process via libp11: OpenSSL 4.0 has no pkcs11 engine for `openssl req`, and the CLI
    // segfaults on exit when signing through the pkcs11-provider instead.
    // ctx is never unloaded on purpose: PKCS11_CTX_unload() calls C_Finalize(), which would tear
    // the module down for every later PKCS#11 user in this test process.
    PKCS11_CTX* ctx = PKCS11_CTX_new();
    if (PKCS11_CTX_load(ctx, hsm_->module_.c_str()) != 0) {
      throw std::runtime_error("Couldn't load PKCS11 module " + hsm_->module_ + ": " +
                               ERR_error_string(ERR_get_error(), nullptr));
    }

    PKCS11_SLOT* slots;
    unsigned int nslots;
    if (PKCS11_enumerate_slots(ctx, &slots, &nslots) != 0) {
      throw std::runtime_error("Couldn't enumerate PKCS11 slots: " +
                               std::string(ERR_error_string(ERR_get_error(), nullptr)));
    }
    PKCS11_SLOT* slot = nullptr;
    for (unsigned int i = 0; i < nslots; i++) {
      if ((slots[i].token != nullptr) && hsm_->label_ == slots[i].token->label) {
        slot = &slots[i];
        break;
      }
    }
    if (slot == nullptr) {
      throw std::runtime_error("Couldn't find a PKCS11 token with label " + hsm_->label_);
    }
    if (PKCS11_open_session(slot, 1) != 0) {
      throw std::runtime_error("Couldn't open a PKCS11 session: " +
                               std::string(ERR_error_string(ERR_get_error(), nullptr)));
    }
    if (PKCS11_login(slot, 0, hsm_->pin_.c_str()) != 0) {
      throw std::runtime_error("Couldn't login to the PKCS11 token: " +
                               std::string(ERR_error_string(ERR_get_error(), nullptr)));
    }

    PKCS11_KEY* keys;
    unsigned int nkeys;
    if (PKCS11_enumerate_keys(slot->token, &keys, &nkeys) != 0) {
      throw std::runtime_error("Couldn't enumerate PKCS11 private keys: " +
                               std::string(ERR_error_string(ERR_get_error(), nullptr)));
    }
    PKCS11_KEY* key = nullptr;
    for (unsigned int i = 0; i < nkeys; i++) {
      if ((keys[i].label != nullptr) && label == keys[i].label) {
        key = &keys[i];
        break;
      }
    }
    if (key == nullptr) {
      throw std::runtime_error("Couldn't find a PKCS11 private key with label " + label);
    }
    StructGuard<EVP_PKEY> pkey(PKCS11_get_private_key(key), EVP_PKEY_free);
    if (pkey == nullptr) {
      throw std::runtime_error("Couldn't load private key " + label + " via libp11: " +
                               std::string(ERR_error_string(ERR_get_error(), nullptr)));
    }

    StructGuard<X509_REQ> req(X509_REQ_new(), X509_REQ_free);
    // OpenSSL 4.0 made this getter return const X509_NAME*, but the returned name is still the
    // CSR's own mutable subject name field - cast away the const to keep populating it in place.
    auto* name = const_cast<X509_NAME*>(X509_REQ_get_subject_name(req.get()));
    X509_NAME_add_entry_by_txt(name, "C", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("SP"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "ST", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("MALAGA"), -1, -1, 0);
    X509_NAME_add_entry_by_txt(name, "CN", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("DeviceHSM"), -1, -1,
                               0);
    X509_NAME_add_entry_by_txt(name, "OU", MBSTRING_ASC, reinterpret_cast<const unsigned char*>("Factory"), -1, -1,
                               0);

    StructGuard<STACK_OF(X509_EXTENSION)> exts(sk_X509_EXTENSION_new_null(),
                                               [](STACK_OF(X509_EXTENSION)* e) { sk_X509_EXTENSION_pop_free(e, X509_EXTENSION_free); });
    sk_X509_EXTENSION_push(exts.get(),
                           X509V3_EXT_conf_nid(nullptr, nullptr, NID_key_usage,
                                                const_cast<char*>("critical,digitalSignature")));
    sk_X509_EXTENSION_push(exts.get(), X509V3_EXT_conf_nid(nullptr, nullptr, NID_ext_key_usage,
                                                           const_cast<char*>("critical,clientAuth")));
    X509_REQ_add_extensions(req.get(), exts.get());

    X509_REQ_set_pubkey(req.get(), pkey.get());
    if (X509_REQ_sign(req.get(), pkey.get(), EVP_sha256()) <= 0) {
      throw std::runtime_error("X509_REQ_sign failed: " + std::string(ERR_error_string(ERR_get_error(), nullptr)));
    }

    StructGuard<BIO> bio(BIO_new(BIO_s_mem()), BIO_vfree);
    PEM_write_bio_X509_REQ(bio.get(), req.get());
    char* pem_data = nullptr;
    auto pem_len = BIO_get_mem_data(bio.get(), &pem_data);  // NOLINT(google-runtime-int)
    out.assign(pem_data, static_cast<size_t>(pem_len));

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

