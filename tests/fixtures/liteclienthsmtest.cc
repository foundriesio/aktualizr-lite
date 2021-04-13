#include "liteclienttest.cc"
namespace fixtures {
#include "liteclient/rootcapki.cc"
#include "liteclient/serverpki.cc"
#include "liteclient/softhsm.cc"
#include "liteclient/devicehsm.cc"
#include "liteclient/subscriberpki.cc"

class ClientHSMTest : public ClientTest {

 private:
  Config liteClientHsmConfig(boost::optional<std::vector<std::string>> apps,
                             const std::string& compose_apps_root) {
    Config conf;

    conf.tls.pkey_source = CryptoSource::kPkcs11;
    conf.tls.cert_source = CryptoSource::kPkcs11;
    conf.tls.ca_source = CryptoSource::kFile;
    conf.tls.server = device_gateway_.getTreeUri();

    conf.p11.tls_clientcert_id = subscriber_->certId_;
    conf.p11.tls_pkey_id = subscriber_->keyId_;
    conf.p11.module = { hsm_->module_.c_str() };
    conf.p11.pass = hsm_->pin_;

    conf.import.base_path = hsm_->path_;
    conf.import.tls_cacert_path = { "ca.crt" };
    conf.import.tls_clientcert_path = { "" };
    conf.import.tls_pkey_path = { "" };

    conf.provision.server = device_gateway_.getTreeUri();
    conf.provision.primary_ecu_hardware_id = hw_id;

    conf.storage.tls_cacert_path = { "ca.crt" };
    conf.storage.sqldb_path = { "sql.db" };
    conf.storage.tls_clientcert_path = { "" };
    conf.storage.tls_pkey_path = { "" };
    conf.storage.path = test_dir_.Path();

    conf.bootloader.reboot_command = "/bin/true";
    conf.bootloader.reboot_sentinel_dir = conf.storage.path; // note

    conf.uptane.repo_server = device_gateway_.getTufRepoUri();

    conf.pacman.type = ComposeAppManager::Name;
    conf.pacman.ostree_server = device_gateway_.getOsTreeUri();
    conf.pacman.sysroot = sys_repo_.getPath();
    conf.pacman.os = os;
    conf.pacman.extra["booted"] = "0";
    conf.pacman.extra["compose_apps_root"] = compose_apps_root.empty() ?
      (test_dir_.Path() / "compose-apps").string() : compose_apps_root;

    if (!!apps) {
      conf.pacman.extra["compose_apps"] = boost::algorithm::join(*apps, ",");
    }

    return conf;
  }

  void addTarget(Config &conf, InitialVersion type) {
    /*
     * Sample LMP/OE generated installed_version file
     * {
     *   "raspberrypi4-64-lmp" {
     *      "hashes": {
     *        "sha256": "cbf23f479964f512ff1d0b01a688d096a670d1d099c1ee3d46baea203e7ef4ab"
     *      },
     *      "is_current": true,
     *      "custom": {
     *        "targetFormat": "OSTREE",
     *        "name": "raspberrypi4-64-lmp",
     *        "version": "1",
     *        "hardwareIds": [
     *                       "raspberrypi4-64"
     *                       ],
     *        "lmp-manifest-sha": "0db09a7e9bac87ef2127e5be8d11f23b3e18513c",
     *        "arch": "aarch64",
     *        "image-file": "lmp-factory-image-raspberrypi4-64.wic.gz",
     *        "meta-subscriber-overrides-sha": "43093be20fa232ef5fe17135115bac4327b501bd",
     *        "tags": [
     *                "master"
     *                ],
     *        "docker_compose_apps": {
     *          "app-01": {
     *            "uri":
     * "hub.foundries.io/msul-dev01/app-06@sha256:2e7b8bc87c67f6042fb88e575a1c73bf70d114f3f2fd1a7aeb3d1bf3b6a0737f"
     *          },
     *          "app-02": {
     *            "uri":
     * "hub.foundries.io/msul-dev01/app-05@sha256:267b14e2e0e98d7e966dbd49bddaa792e5d07169eb3cf2462bbbfecac00f46ef"
     *          }
     *        },
     *        "containers-sha": "a041e7a0aa1a8e73a875b4c3fdf9a418d3927894"
     *     }
     *  }
     */

    Json::Value install;
    // corrupted1 will invalidate the sysroot_hash_ sha256
    install["hashes"]["sha256"] = sysroot_hash_ + (type == InitialVersion::kCorrupted1 ? "DEADBEEF" : "");
    install["is_current"] = true;
    install["custom"]["name"] = hw_id + "-" + os;
    install["custom"]["version"] = "1";
    install["custom"]["hardwareIds"] = hw_id;
    install["custom"]["targetFormat"] = "OSTREE";
    install["custom"]["arch"] = "aarch64";
    install["custom"]["image-file"] = "lmp-factory-image-raspberrypi4-64.wic.gz";
    install["custom"]["tags"] = "master";

    Json::Value version;
    initial_target_ = Uptane::Target{hw_id + "-" + os + "-" + "1", install};
    version[initial_target_.filename()] = install;

    Utils::writeFile(conf.import.base_path / "installed_versions",
                     (type == InitialVersion::kCorrupted2)?
                     "deadbeef\t\ncorrupted file\n\n" : Utils::jsonToCanonicalStr(version), true);

    getTufRepo().addTarget(initial_target_.filename(), initial_target_.sha256Hash(), hw_id, "1");
  }

 protected:
  static void SetUpTestSuite() {
    /**
     * Actions: prepares the RootCA, HSM, PKI Server and Subscriber.
     *
     * Creates shared directory for all tests to store PKI and configuration files
     *   - ca.key, ca.crt
     *   - server.csr, server.crt, pkey.pem
     *   - device.csr, device.crt
     *   - ssl.conf, softhsm2.conf
     */
    boost::filesystem::path path = boost::filesystem::temp_directory_path() / boost::filesystem::unique_path();
    Utils::createDirectories(path, S_IRWXU);
    // local to the platform: SoftHSM token
    hsm_ = new SoftHsm(path.string(), "/softhsm2.conf");
    // external to the platform: rootCA authority
    RootCaPKI ca(path.string(), "/ca.key", "/ca.crt");
    // local to the platform: the device hsm interface
    DeviceHsm device(hsm_, ca, "/ssl.conf");
    // external to the platform: the file server
    ServerPKI server(path.string(), ca, "/server.csr", "/server.crt", "/pkey.pem");
    // local to the platform: the PKI subscriber
    subscriber_ = new SubscriberPKI(device, "01", "03", "tls", "/device.csr", "/device.crt");
    LOG_INFO << "PKI created, certificates directory: " << path.string();
  }

  ClientHSMTest() : ClientTest(hsm_->path_) {
  }

  std::shared_ptr<LiteClient> createLiteClient(const std::shared_ptr<AppEngine>& app_engine,
                                               InitialVersion version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               const std::string& compose_apps_root = "") {
    /* setup short list */
    app_shortlist_ = apps;

    Config conf = liteClientHsmConfig(apps, compose_apps_root);

    if (version == InitialVersion::kOn ||
        version == InitialVersion::kCorrupted1 ||
        version == InitialVersion::kCorrupted2) {
      addTarget(conf, version);
    }

    return std::make_shared<LiteClient>(conf, app_engine);
  }

 protected:
  static SubscriberPKI* subscriber_;
  static SoftHsm* hsm_;
};

SubscriberPKI* ClientHSMTest::subscriber_;
SoftHsm* ClientHSMTest::hsm_;
} // namespace fixtures
