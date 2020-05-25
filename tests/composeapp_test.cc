#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "http/httpclient.h"
#include "test_utils.h"
#include "utilities/utils.h"
#include "http/httpinterface.h"

#include "composeappmanager.h"


class FakeRegistry {
  public:
    FakeRegistry(const std::string base_url, boost::filesystem::path root_dir):base_url_{base_url}, root_dir_{root_dir} {
    }

    std::string addApp(const std::string& app_repo, const std::string& app_name,
                       const std::string file_name, std::string app_content) {
      // TODO compose a proper docker compose app here (bunch of files)
      auto docker_flie = root_dir_ / app_name / file_name;
      Utils::writeFile(docker_flie, app_content);
      tgz_path_ = root_dir_ / app_name / (app_name + ".tgz");
      std::string stdout_msg;
      boost::process::system("tar -czf " + tgz_path_.string() + " " + file_name, boost::process::start_dir = (root_dir_ / app_name));
      std::string tgz_content = Utils::readFile(tgz_path_);
      auto hash = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(tgz_content)));
      // TODO: it should be in ComposeApp::Manifest::Manifest()
      manifest_["annotations"]["compose-app"] = "v1";
      manifest_["layers"][0]["digest"] = "sha256:" + hash;
      manifest_["layers"][0]["size"] = tgz_content.size();
      manifest_hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::jsonToCanonicalStr(manifest_))));
      // app URI
      auto app_uri = base_url_ + app_repo + '/' + app_name + '@' + "sha256:" + manifest_hash_;
      return app_uri;
    }

    std::string getManifest() const { return Utils::jsonToCanonicalStr(manifest_); }
    std::string getShortManifestHash() const { return manifest_hash_.substr(0, 7); }
    std::string getArchiveContent() const { return Utils::readFile(tgz_path_); }

  private:
    const std::string base_url_;
    boost::filesystem::path root_dir_;
    Json::Value manifest_;
    std::string manifest_hash_;
    boost::filesystem::path tgz_path_;
};

class FakeOtaClient: public HttpInterface {
  public:
    FakeOtaClient(FakeRegistry& registry, const std::vector<std::string>* headers = nullptr):
      registry_{registry}, headers_{headers} {}

  public:
    HttpResponse get(const std::string &url, int64_t maxsize) override {
      std::string resp;
      if (0 == url.find("https://hub.foundries.io/token-auth/")) {
        resp = "{\"token\":\"token\"}";
      } else if (0 == url.find("https://hub.foundries.io/v2/")) {
        resp = registry_.getManifest();
      } else {
        resp = "{\"Secret\":\"secret\",\"Username\":\"test-user\"}";
      }
     return HttpResponse(resp, 200, CURLE_OK, "");
    }

    HttpResponse post(const std::string &url, const std::string &content_type, const std::string &data) override {}

    HttpResponse post(const std::string &url, const Json::Value &data) override {}

    HttpResponse put(const std::string &url, const std::string &content_type, const std::string &data) override {}

    HttpResponse put(const std::string &url, const Json::Value &data) override {}

    HttpResponse download(const std::string &url, curl_write_callback write_cb,
                                  curl_xferinfo_callback progress_cb, void *userp, curl_off_t from) override {

      std::string data{registry_.getArchiveContent()};
      write_cb(const_cast<char*>(data.c_str()), data.size(), 1, userp);

      return HttpResponse("resp", 200, CURLE_OK, "");
    }

    std::future<HttpResponse> downloadAsync(const std::string &url, curl_write_callback write_cb,
                                                    curl_xferinfo_callback progress_cb, void *userp, curl_off_t from,
                                                    CurlHandler *easyp) override {


    }

    void setCerts(const std::string &ca, CryptoSource ca_source, const std::string &cert,
                          CryptoSource cert_source, const std::string &pkey, CryptoSource pkey_source) override {}

 private:
  FakeRegistry& registry_;
  const std::vector<std::string>* headers_;

};

static boost::filesystem::path test_sysroot;

static struct {
  int serial{0};
  std::string rev;
} ostree_deployment;
static std::string new_rev;

extern "C" OstreeDeployment* ostree_sysroot_get_booted_deployment(OstreeSysroot* self) {
  (void)self;
  static GObjectUniquePtr<OstreeDeployment> dep;

  dep.reset(ostree_deployment_new(0, "dummy-os", ostree_deployment.rev.c_str(), ostree_deployment.serial,
                                  ostree_deployment.rev.c_str(), ostree_deployment.serial));
  return dep.get();
}

extern "C" const char* ostree_deployment_get_csum(OstreeDeployment* self) {
  (void)self;
  return ostree_deployment.rev.c_str();
}

static void progress_cb(const Uptane::Target& target, const std::string& description, unsigned int progress) {
  (void)description;
  LOG_INFO << "progress_cb " << target << " " << progress;
}

TEST(ComposeApp, Config) {
  Config config;
  config.pacman.type = PACKAGE_MANAGER_COMPOSEAPP;
  config.pacman.sysroot = test_sysroot.string();
  config.pacman.extra["compose_apps_root"] = "apps-root";
  config.pacman.extra["compose_apps"] = "app1 app2";
  config.pacman.extra["docker_compose_bin"] = "compose";

  ComposeAppConfig cfg(config.pacman);
  ASSERT_TRUE(cfg.docker_prune);
  ASSERT_EQ(2, cfg.apps.size());
  ASSERT_EQ("app1", cfg.apps[0]);
  ASSERT_EQ("app2", cfg.apps[1]);
  ASSERT_EQ("apps-root", cfg.apps_root);
  ASSERT_EQ("compose", cfg.compose_bin);

  config.pacman.extra["docker_prune"] = "0";
  cfg = ComposeAppConfig(config.pacman);
  ASSERT_FALSE(cfg.docker_prune);

  config.pacman.extra["docker_prune"] = "FALSE";
  cfg = ComposeAppConfig(config.pacman);
  ASSERT_FALSE(cfg.docker_prune);
}

struct TestClient {
  TestClient(const char* apps, const Uptane::Target* installedTarget = nullptr,
             const std::shared_ptr<HttpInterface>& ota_lite_client = nullptr,
             RegistryClient::HttpClientFactory registry_http_client_factory = nullptr) {
    tempdir = std_::make_unique<TemporaryDirectory>();

    Config config;
    config.logger.loglevel = 1;
    config.pacman.type = PACKAGE_MANAGER_COMPOSEAPP;
    config.bootloader.reboot_sentinel_dir = tempdir->Path();
    config.pacman.sysroot = test_sysroot.string();
    config.pacman.extra["compose_apps_root"] = (*tempdir / "apps").native();
    config.pacman.extra["compose_apps"] = apps;
    config.pacman.extra["docker_compose_bin"] = "src/compose_fake.sh";
    config.pacman.extra["docker_prune"] = "0";
    config.storage.path = tempdir->Path();

    config.pacman.extra["registry_auth_creds_endpoint"] = "http://hub-creds/";
    config.pacman.extra["registry_base_url"] = "http://";

    storage = INvStorage::newStorage(config.storage);
    if (installedTarget != nullptr) {
      storage->savePrimaryInstalledVersion(*installedTarget, InstalledVersionUpdateMode::kCurrent);
    }
    pacman = std_::make_unique<ComposeAppManager>(config.pacman, config.bootloader, storage, ota_lite_client, registry_http_client_factory);
    keys = std_::make_unique<KeyManager>(storage, config.keymanagerConfig());
    fetcher = std_::make_unique<Uptane::Fetcher>(config, std::make_shared<HttpClient>());
  }

  std::unique_ptr<TemporaryDirectory> tempdir;
  std::shared_ptr<INvStorage> storage;
  std::unique_ptr<ComposeAppManager> pacman;
  std::unique_ptr<KeyManager> keys;
  std::unique_ptr<Uptane::Fetcher> fetcher;
};

TEST(ComposeApp, getApps) {
  TemporaryDirectory dir;
  auto mgr = TestClient("app1 app2").pacman;

  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "n/a";
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = "N/A";
  Uptane::Target target("pull", target_json);

  auto apps = mgr->getApps(target);
  ASSERT_EQ(2, apps.size());
  ASSERT_EQ("app1", apps[0].first);
  ASSERT_EQ("n/a", apps[0].second);
  ASSERT_EQ("app2", apps[1].first);
  ASSERT_EQ("N/A", apps[1].second);
}

TEST(ComposeApp, fetch) {
  TemporaryDirectory tmp_dir;
  FakeRegistry registry{"https://hub.io/", tmp_dir.Path()};
  auto fake_ota_client = std::make_shared<FakeOtaClient>(registry);
  RegistryClient::HttpClientFactory registry_fake_http_client_factory = [&registry](const std::vector<std::string>* headers) {
    return std::make_shared<FakeOtaClient>(registry, headers);
  };
  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "n/a";
  const std::string app_file_name = "docker-compose.yml";
  const std::string app_content = "lajdalsjdlasjflkjasldjaldlasdl89749823748jsdhfjshdfjk89273498273jsdkjkdfjkdsfj928";
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2", app_file_name, app_content);
  Uptane::Target target("pull", target_json);

  TestClient client("app2 doesnotexist", nullptr, fake_ota_client, registry_fake_http_client_factory);  // only app2 can be fetched
  bool result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_TRUE(result);
  const std::string expected_file = (client.pacman->cfg_.apps_root / "app2"/ app_file_name).string();
  ASSERT_TRUE(boost::filesystem::exists(expected_file));
  std::string delivered_app_file_content = Utils::readFile(expected_file);
  ASSERT_EQ(delivered_app_file_content, app_content);

  auto output = Utils::readFile(client.tempdir->Path() / "apps/app2/config.log", true);
  ASSERT_EQ("config", output);
  output = Utils::readFile(client.tempdir->Path() / "apps/app2/pull.log", true);
  ASSERT_EQ("pull", output);
  ASSERT_FALSE(boost::filesystem::exists(client.tempdir->Path() / "apps/doesnotexist"));

  // Now do a quick check that we can handle a simple download failure
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = "FAILTEST";
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);
}

TEST(ComposeApp, handleRemovedApps) {
  // Configure a client for app1, app2, and app3
  TestClient client("app1 app2 app3", nullptr);
  auto apps = client.tempdir->Path() / "apps";

  // Create a target for both app2 (app3 is configured, but not in the targets)
  Json::Value target_json;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "";
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = "";
  Uptane::Target target("pull", target_json);

  // Make app2 and app3 look like they are installed:
  boost::filesystem::create_directories(apps / "app2");
  boost::filesystem::create_directories(apps / "app3");
  // Make an app we aren't configured for:
  boost::filesystem::create_directories(apps / "BAD");

  client.pacman->handleRemovedApps(target);
  ASSERT_FALSE(boost::filesystem::exists(apps / "BAD"));
  ASSERT_FALSE(boost::filesystem::exists(apps / "app3"));
  ASSERT_TRUE(boost::filesystem::exists(apps / "app2"));
}

TEST(ComposeApp, install) {
  // Trick system into not doing an OSTreeManager install
  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  ostree_deployment.serial = 1;
  ostree_deployment.rev = sha;

  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "";
  Uptane::Target target("pull", target_json);

  TestClient client("app1", &target);

  // We are't doing a fetch, so we have to make this directory so that the
  // compose_fake script can run:
  boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");
  ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install(target).result_code.num_code);
  std::string output = Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true);
  ASSERT_EQ("up --remove-orphans -d", output);
}

#ifndef __NO_MAIN__
int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);

  if (argc != 2) {
    std::cerr << "Error: " << argv[0] << " requires the path to an OSTree sysroot as an input argument.\n";
    return EXIT_FAILURE;
  }

  TemporaryDirectory temp_dir;
  // Utils::copyDir doesn't work here. Complaints about non existent symlink path
  int r = system((std::string("cp -r ") + argv[1] + std::string(" ") + temp_dir.PathString()).c_str());
  if (r != 0) {
    return -1;
  }
  test_sysroot = (temp_dir.Path() / "ostree_repo").string();

  return RUN_ALL_TESTS();
}
#endif
