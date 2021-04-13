/**
*  PKI subscriber (LiteClient) registering with the HSM via the deviceHSM
*
*/
class SubscriberPKI {
 public:
  SubscriberPKI(DeviceHsm deviceHsm, std::string keyId, std::string certId, std::string keyLabel, std::string csr,
                std::string crt)
      : keyId_(std::move(keyId)),
        certId_(std::move(certId)),
        keyLabel_(std::move(keyLabel)),
        csr_{std::move(csr)},
        crt_{std::move(crt)}
  {
    deviceHsm.createKey(keyId_, keyLabel_);
    deviceHsm.createCsr(keyLabel_, csr_);
    deviceHsm.createCrt(csr_, crt_);
    deviceHsm.importCrt(crt_, certId_);
    /* enable for debug */
    deviceHsm.listInfo();
  }

 public:
  std::string keyId_;
  std::string certId_;

 private:
  std::string keyLabel_;
  std::string csr_;
  std::string crt_;
};


