#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "http/httpclient.h"
#include "test_utils.h"
#include "utilities/utils.h"
#include "http/httpinterface.h"

#include "composeappmanager.h"
#include "composeapp.h"


class FakeRegistry {
  public:
    FakeRegistry(const std::string& auth_url, const std::string& base_url, const boost::filesystem::path& root_dir):
      auth_url_{auth_url}, base_url_{base_url}, root_dir_{root_dir} {}

    using ManifestPostProcessor = std::function<void(Json::Value&, std::string&)>;

    std::string addApp(const std::string& app_repo, const std::string& app_name,
                       ManifestPostProcessor manifest_post_processor = nullptr,
                       const std::string file_name = "docker-compose.yml",
                       std::string app_content = "some fake content qwertyuiop 1231313123123123") {
      // TODO compose a proper docker compose app here (bunch of files)
      auto docker_flie = root_dir_ / app_name / file_name;
      Utils::writeFile(docker_flie, app_content);
      tgz_path_ = root_dir_ / app_name / (app_name + ".tgz");
      std::string stdout_msg;
      boost::process::system("tar -czf " + tgz_path_.string() + " " + file_name, boost::process::start_dir = (root_dir_ / app_name));
      std::string tgz_content = Utils::readFile(tgz_path_);
      auto hash = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(tgz_content)));
      // TODO: it should be in ComposeApp::Manifest::Manifest()
      manifest_.clear();
      manifest_["annotations"]["compose-app"] = "v1";
      manifest_["layers"][0]["digest"] = "sha256:" + hash;
      manifest_["layers"][0]["size"] = tgz_content.size();
      manifest_hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::jsonToCanonicalStr(manifest_))));
      if (manifest_post_processor) {
        manifest_post_processor(manifest_, hash);
        manifest_hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::jsonToCanonicalStr(manifest_))));
      }
      archive_name_ = hash.substr(0,7) + '.' + app_name + ".tgz";
      // app URI
      auto app_uri = base_url_ + '/' + app_repo + '/' + app_name + '@' + "sha256:" + manifest_hash_;
      return app_uri;
    }

    const std::string& authURL() const {return auth_url_; }
    const std::string& baseURL() const {return base_url_; }
    Json::Value& manifest() { return manifest_; }
    const std::string& archiveName() const {return archive_name_; }
    std::string getManifest() const { was_manifest_requested_ = true; return Utils::jsonToCanonicalStr(manifest_); }
    std::string getShortManifestHash() const { return manifest_hash_.substr(0, 7); }
    std::string getArchiveContent() const { return Utils::readFile(tgz_path_); }

    bool wasManifestRequested() const { return was_manifest_requested_; }

  private:
    const std::string auth_url_;
    const std::string base_url_;
    boost::filesystem::path root_dir_;
    Json::Value manifest_;
    std::string manifest_hash_;
    boost::filesystem::path tgz_path_;
    std::string archive_name_;
    mutable bool was_manifest_requested_{false};
};

class FakeOtaClient: public HttpInterface {
  public:
    FakeOtaClient(FakeRegistry* registry, const std::vector<std::string>* headers = nullptr):
      registry_{registry}, headers_{headers} {}

  public:
    HttpResponse get(const std::string &url, int64_t maxsize) override {
      assert(registry_);
      std::string resp;
      if (std::string::npos != url.find(registry_->baseURL() + "/token-auth/")) {
        resp = "{\"token\":\"token\"}";
      } else if (std::string::npos != url.find(registry_->baseURL() + "/v2/")) {
        resp = registry_->getManifest();
      } else if (url == registry_->authURL()) {
        resp = "{\"Secret\":\"secret\",\"Username\":\"test-user\"}";
      } else {
        return HttpResponse(resp, 401, CURLE_OK, "");
      }
     return HttpResponse(resp, 200, CURLE_OK, "");
    }

    HttpResponse download(const std::string &url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb, void *userp, curl_off_t from) override {

      (void)url;
      (void)progress_cb;
      (void)from;

      assert(registry_);
      std::string data{registry_->getArchiveContent()};
      write_cb(const_cast<char*>(data.c_str()), data.size(), 1, userp);

      return HttpResponse("resp", 200, CURLE_OK, "");
    }

    std::future<HttpResponse> downloadAsync(const std::string&, curl_write_callback, curl_xferinfo_callback, void*, curl_off_t, CurlHandler*) override {
      std::promise<HttpResponse> resp_promise;
      resp_promise.set_value(HttpResponse("", 500, CURLE_OK, ""));
      return resp_promise.get_future();
    }
    HttpResponse post(const std::string&, const std::string&, const std::string&) override { return HttpResponse("", 500, CURLE_OK, ""); }
    HttpResponse post(const std::string&, const Json::Value&) override { return HttpResponse("", 500, CURLE_OK, ""); }
    HttpResponse put(const std::string&, const std::string&, const std::string&) override { return HttpResponse("", 500, CURLE_OK, ""); }
    HttpResponse put(const std::string&, const Json::Value&) override { return HttpResponse("", 500, CURLE_OK, ""); }
    void setCerts(const std::string&, CryptoSource, const std::string&, CryptoSource, const std::string&, CryptoSource) override {}

 private:
  FakeRegistry* registry_;
  const std::vector<std::string>* headers_;
};

static boost::filesystem::path test_sysroot;

static void progress_cb(const Uptane::Target& target, const std::string& description, unsigned int progress) {
  (void)description;
  LOG_INFO << "progress_cb " << target << " " << progress;
}

TEST(ComposeApp, Config) {
  Config config;
  config.pacman.type = ComposeAppManager::Name;
  config.pacman.sysroot = test_sysroot.string();
  config.pacman.extra["compose_apps_root"] = "apps-root";
  config.pacman.extra["compose_apps"] = "app1 app2";
  config.pacman.extra["docker_compose_bin"] = "compose";

  ComposeAppManager::Config cfg(config.pacman);
  ASSERT_TRUE(cfg.docker_prune);
  ASSERT_EQ(2, cfg.apps.size());
  ASSERT_EQ("app1", cfg.apps[0]);
  ASSERT_EQ("app2", cfg.apps[1]);
  ASSERT_EQ("apps-root", cfg.apps_root);
  ASSERT_EQ("compose", cfg.compose_bin);

  config.pacman.extra["docker_prune"] = "0";
  cfg = ComposeAppManager::Config(config.pacman);
  ASSERT_FALSE(cfg.docker_prune);

  config.pacman.extra["docker_prune"] = "FALSE";
  cfg = ComposeAppManager::Config(config.pacman);
  ASSERT_FALSE(cfg.docker_prune);
}

class TestSysroot: public OSTree::Sysroot {
 public:
  using Hasher = std::function<std::string()>;

 public:
    TestSysroot(Hasher hasher, const std::string& sysroot_path):
      OSTree::Sysroot(sysroot_path, false),
      hasher_{std::move(hasher)} {}

    virtual std::string getCurDeploymentHash() const {
      return hasher_();
    }

 private:
  Hasher hasher_;
};


struct TestClient {
  TestClient(const char* apps, const Uptane::Target* installedTarget = nullptr,
             FakeRegistry* registry = nullptr, std::string ostree_server_url = "",
             bool force_update = false, TestSysroot::Hasher sysroot_hasher = nullptr) {
    tempdir = std_::make_unique<TemporaryDirectory>();

    config.logger.loglevel = 1;
    config.pacman.type = ComposeAppManager::Name;
    config.bootloader.reboot_sentinel_dir = tempdir->Path();
    config.pacman.sysroot = test_sysroot.string();
    // this a name/title of a group of ostree-based rootfs deployments that share /var, aka "stateroot" or "osname"
    // aktualizr supports just one stateroot, for test purposes a tool ./aktualizr/tests/ostree-scripts/makephysical.sh
    // creates an ostree sysroot/rootfs with stateroot called "dummy-os" at build time and put it in build/aktualizr/ostree_repo/
    // many tests use this build-time generated rootfs for testing purposes and should be aware of the hardcoded stateroot name
    config.pacman.os = "dummy-os";
    apps_root = config.pacman.extra["compose_apps_root"] = (*tempdir / "apps").native();
    config.pacman.extra["compose_apps"] = apps;
    config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
    config.pacman.extra["docker_prune"] = "0";
    config.pacman.extra["force_update"] = force_update?"1":"0";
    config.storage.path = tempdir->Path();

    if (!ostree_server_url.empty()) {
      config.pacman.ostree_server = ostree_server_url;
    }

    storage = INvStorage::newStorage(config.storage);
    if (installedTarget != nullptr) {
      storage->savePrimaryInstalledVersion(*installedTarget, InstalledVersionUpdateMode::kCurrent);
    }

    if (registry) {
      http_client = std::make_shared<FakeOtaClient>(registry);
      registry_fake_http_client_factory = [registry](const std::vector<std::string>* headers) {
        return std::make_shared<FakeOtaClient>(registry, headers);
      };
    }
    sysroot = (sysroot_hasher == nullptr) ? std::make_shared<OSTree::Sysroot>(config.pacman.sysroot.string(), false) :
                                                 std::make_shared<TestSysroot>(sysroot_hasher, config.pacman.sysroot.string());
    pacman = std::make_shared<ComposeAppManager>(config.pacman, config.bootloader, storage, http_client,
                                                  sysroot, registry_fake_http_client_factory);
    keys = std_::make_unique<KeyManager>(storage, config.keymanagerConfig());
    fetcher = std_::make_unique<Uptane::Fetcher>(config, std::make_shared<HttpClient>());
  }

  const boost::filesystem::path getRebootSentinel() const {
    return config.bootloader.reboot_sentinel_dir / config.bootloader.reboot_sentinel_name;
  }

  void fakeReboot() {
    boost::filesystem::remove(getRebootSentinel());
    pacman.reset(new ComposeAppManager(config.pacman, config.bootloader, storage, http_client, sysroot, registry_fake_http_client_factory));
  }

  Config config;
  std::unique_ptr<TemporaryDirectory> tempdir;
  std::shared_ptr<INvStorage> storage;
  std::shared_ptr<ComposeAppManager> pacman;
  std::unique_ptr<KeyManager> keys;
  std::unique_ptr<Uptane::Fetcher> fetcher;
  boost::filesystem::path apps_root;

  std::shared_ptr<HttpInterface> http_client;
  std::shared_ptr<OSTree::Sysroot> sysroot;
  Docker::RegistryClient::HttpClientFactory registry_fake_http_client_factory;
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
  const std::string ostree_server_url{"https://my-ota/treehub"};
  TemporaryDirectory tmp_dir;
  FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "n/a";
  const std::string app_file_name = "docker-compose.yml";
  const std::string app_content = "lajdalsjdlasjflkjasldjaldlasdl89749823748jsdhfjshdfjk89273498273jsdkjkdfjkdsfj928";
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                                                                nullptr, app_file_name, app_content);
  Uptane::Target target("pull", target_json);

  TestClient client("app2 doesnotexist", nullptr, &registry, ostree_server_url);  // only app2 can be fetched
  bool result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_TRUE(result);
  const std::string expected_file = (client.apps_root / "app2"/ app_file_name).string();
  ASSERT_TRUE(boost::filesystem::exists(expected_file));
  std::string delivered_app_file_content = Utils::readFile(expected_file);
  ASSERT_EQ(delivered_app_file_content, app_content);
  ASSERT_FALSE(boost::filesystem::exists(client.apps_root / "app2"/ registry.archiveName()));

  auto output = Utils::readFile(client.tempdir->Path() / "apps/app2/config.log", true);
  ASSERT_EQ("config", output);
  output = Utils::readFile(client.tempdir->Path() / "apps/app2/pull.log", true);
  ASSERT_EQ("pull --no-parallel", output);
  ASSERT_FALSE(boost::filesystem::exists(client.tempdir->Path() / "apps/doesnotexist"));
}

TEST(ComposeApp, fetchNegative) {
  TemporaryDirectory tmp_dir;

  FakeRegistry registry{Docker::RegistryClient::DefAuthCredsEndpoint, "https://hub.io/", tmp_dir.Path()};
  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "n/a";

  Uptane::Target target("pull", target_json);
  TestClient client("app2", nullptr, &registry);

  // Now do a quick check that we can handle a simple download failure
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = "FAILTEST";
  target = Uptane::Target("pull", target_json);
  bool result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid App URI
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = "hub.io/test_repo/app2sha256:712329f5d298ccc144f2d1c8b071cc277dcbe77796d8d3a805b69dd09aa486dc";
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid App Manifest: no version
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                                  [](Json::Value& manifest, std::string&) {
                                                    manifest["annotations"].clear();
                                                  });
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid App Manifest: unsupported version
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                                          [](Json::Value& manifest, std::string&) {
                                                            manifest["annotations"]["compose-app"] = "v0";
                                                          });
  registry.manifest()["annotations"]["compose-app"] = "v0";
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid App Manifest: no archive/blob layer
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                                          [](Json::Value& manifest, std::string&) {
                                                            manifest["layers"].clear();
                                                          });
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid App Manifest: invalid hash caused by manifest altering
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2");
  registry.manifest()["custom"]["some_filed"] = "some_value";
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid archive hash
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                      [](Json::Value& manifest, std::string& hash) {
                                        manifest["layers"][0]["digest"] = "sha256:" + hash.replace(2, 3, "123");
                                      });
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid archive size: received more data than specified in the manifest
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                      [](Json::Value& manifest, std::string&) {
                                        manifest["layers"][0]["size"] = manifest["layers"][0]["size"].asUInt() - 1;
                                      });
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Invalid archive size: received less data than specified in the manifest
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                        [](Json::Value& manifest, std::string&) {
                                          manifest["layers"][0]["size"] = manifest["layers"][0]["size"].asUInt() + 1;
                                        });
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);


  // Manifest size exceeds maximum allowed size (RegistryClient::ManifestMaxSize)
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                        [](Json::Value& manifest, std::string&) {
                                          manifest["layers"][1]["some_value"] = std::string(Docker::RegistryClient::ManifestMaxSize + 1, 'f');
                                        });
  target = Uptane::Target("pull", target_json);
  result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_FALSE(result);

  // Archive size exceeds maximum available storage space
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = registry.addApp("test_repo", "app2",
                                        [](Json::Value& manifest, std::string&) {
                                          manifest["layers"][0]["size"] = std::numeric_limits<size_t>::max();
                                        });
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

TEST(ComposeApp, installApp) {
  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);

  Json::Value installed_target_json;
  installed_target_json["hashes"]["sha256"] = sha;

  // TODO: consider using test fixtures, parameterization and avoiding code, literal/constant/params duplication

  // new App installation
  {
    TemporaryDirectory tmp_dir;
    FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

    Uptane::Target installed_target("pull", installed_target_json);
    TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");

    Json::Value target_to_install_json{installed_target_json};
    target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = registry.addApp("test_repo", "app1", nullptr, "myapp");

    ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
    ASSERT_TRUE(registry.wasManifestRequested());
    ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
    ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
    ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

    ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
    ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
  }

  // existing App update (uri/hash does not match)
  {
    TemporaryDirectory tmp_dir;
    FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

    installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "hub.io@factory/app1:sha256:12312312312";
    Uptane::Target installed_target("pull", installed_target_json);

    TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");

    Json::Value target_to_install_json{installed_target_json};
    target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = registry.addApp("test_repo", "app1", nullptr, "myapp");
    // fake that App has been already installed
    boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");

    ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
    ASSERT_TRUE(registry.wasManifestRequested());
    ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
    ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
    ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

    ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
    ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
  }

  // skipping an App update install because already installed
  {
    TemporaryDirectory tmp_dir;
    FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

    auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

    // emulate situation when App is already installed on a system
    installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
    Uptane::Target installed_target("pull", installed_target_json);
    TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
    boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");
    Utils::writeFile(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::ComposeFile, std::string("image: "));
    Utils::writeFile(client.tempdir->Path() / "apps/app1" / "ps.in", std::string("my-image-id\n"));

    Json::Value target_to_install_json{installed_target_json};
    target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

    ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
    // make sure that App manifest wasn't requested from Registry
    ASSERT_FALSE(registry.wasManifestRequested());
    // make sure that docker-compose config and pull were not called
    ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
    ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

    ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
    // make sure that docker-compose up wasn't called
    ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
  }

  // App update, DB thinks that App is installed, but App's directory doesn't exist
  {
    TemporaryDirectory tmp_dir;
    FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

    auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

    // emulate situation when App is already installed on a system but is not present on a file system
    installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
    Uptane::Target installed_target("pull", installed_target_json);
    TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
    //boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");

    Json::Value target_to_install_json{installed_target_json};
    target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

    ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
    ASSERT_TRUE(registry.wasManifestRequested());
    ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
    ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
    ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

    ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
    ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
  }

  // App update, DB thinks that App is installed, but App was not fully installed
  // Initial boot, Target is imported from 'installed_versions to DB and Target
  // fetched from TUF Server are the same, aklite tries to install apps but at some point
  // of data fetching or installation a system is rebooted or aklite is stopped/killed
  {
    // App directory exists but docker-compose.yml is not present
    {
      TemporaryDirectory tmp_dir;
      FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

      auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

      installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
      Uptane::Target installed_target("pull", installed_target_json);
      TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
      boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");

      Json::Value target_to_install_json{installed_target_json};
      target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

      ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
      ASSERT_TRUE(registry.wasManifestRequested());
      ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
      ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
      ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

      ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
      ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
    }

    // App directory exists but images are not fetched
    {
      TemporaryDirectory tmp_dir;
      FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

      auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

      installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
      Uptane::Target installed_target("pull", installed_target_json);
      TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
      boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::ComposeFile, std::string("image: "));
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / "ps.in", std::string(""));

      Json::Value target_to_install_json{installed_target_json};
      target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

      ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
      ASSERT_TRUE(registry.wasManifestRequested());
      ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
      ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
      ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

      ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
      ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
    }

    // App directory exists but images are not fetched and
    // config file has commented `image:` in Compose file
    {
      TemporaryDirectory tmp_dir;
      FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

      auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

      installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
      Uptane::Target installed_target("pull", installed_target_json);
      TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
      boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::ComposeFile, std::string("# image: \n image: "));
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / "ps.in", std::string(""));

      Json::Value target_to_install_json{installed_target_json};
      target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

      ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
      ASSERT_TRUE(registry.wasManifestRequested());
      ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
      ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
      ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

      ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
      ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
    }

    // App is installed and running but Compose file has commented `image:`
    {
      TemporaryDirectory tmp_dir;
      FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

      auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

      installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
      Uptane::Target installed_target("pull", installed_target_json);
      TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
      boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::ComposeFile, std::string("image: \n # image: "));
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / "ps.in", std::string("my-image-id\n"));

      Json::Value target_to_install_json{installed_target_json};
      target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

      ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
      // make sure that App manifest wasn't requested from Registry
      ASSERT_FALSE(registry.wasManifestRequested());
      // make sure that docker-compose config and pull were not called
      ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
      ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

      ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
      // make sure that docker-compose up wasn't called
      ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
    }

    // App that consists of two images is installed and running but Compose file has commented `image:`
    {
      TemporaryDirectory tmp_dir;
      FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

      auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

      installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
      Uptane::Target installed_target("pull", installed_target_json);
      TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub");
      boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::ComposeFile, std::string("#image: \n image: \n # image:\nimage:"));
      Utils::writeFile(client.tempdir->Path() / "apps/app1" / "ps.in", std::string("my-image-id-01\nmy-image-id-02\n"));

      Json::Value target_to_install_json{installed_target_json};
      target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

      ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
      // make sure that App manifest wasn't requested from Registry
      ASSERT_FALSE(registry.wasManifestRequested());
      // make sure that docker-compose config and pull were not called
      ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
      ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

      ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
      // make sure that docker-compose up wasn't called
      ASSERT_EQ("", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
    }

  }

  // forced App update, App is installed, but update was called with 'forced' option
  {
    TemporaryDirectory tmp_dir;
    FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

    auto app_uri = registry.addApp("test_repo", "app1", nullptr, "myapp");

    // emulate situation when App is already installed on a system but is not present on a file system
    installed_target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;
    Uptane::Target installed_target("pull", installed_target_json);
    TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub", true);
    boost::filesystem::create_directories(client.tempdir->Path() / "apps/app1");

    Json::Value target_to_install_json{installed_target_json};
    target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = app_uri;

    ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
    ASSERT_TRUE(registry.wasManifestRequested());
    ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
    ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
    ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

    ASSERT_EQ(data::ResultCode::Numeric::kOk, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
    ASSERT_EQ("up --remove-orphans -d", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));
  }

  // App update if reboot, make sure App is not (re-)started before a system reboot and started just after reboot
  {
    // let the sota client and ostree package manager think that we are booted on some ref/hash
    // that is different from the ref/hash that is actualy deployed in the test ostree sysroot (Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);)
    std::string currently_installed_hash = "00208312202f3e3a02296faa548b9c0f2b1c147c8c29436e42c2f885d9a044f5";
    TestSysroot::Hasher hash_provider{[&currently_installed_hash]{ return currently_installed_hash; } };

    TemporaryDirectory tmp_dir;
    FakeRegistry registry{"https://my-ota/hub-creds/", "hub.io", tmp_dir.Path()};

    Uptane::Target installed_target("pull", installed_target_json);
    TestClient client("app1", &installed_target, &registry, "https://my-ota/treehub", false, hash_provider);

    Json::Value target_to_install_json{installed_target_json};
    target_to_install_json["custom"]["docker_compose_apps"]["app1"]["uri"] = registry.addApp("test_repo", "app1", nullptr, "myapp");

    ASSERT_TRUE(client.pacman->fetchTarget({"pull", target_to_install_json}, *(client.fetcher), *(client.keys), nullptr, nullptr));
    ASSERT_TRUE(registry.wasManifestRequested());
    ASSERT_TRUE(boost::filesystem::exists((client.apps_root / "app1"/ "myapp").string()));
    ASSERT_EQ("config", Utils::readFile(client.tempdir->Path() / "apps/app1/config.log", true));
    ASSERT_EQ("pull --no-parallel", Utils::readFile(client.tempdir->Path() / "apps/app1/pull.log", true));

    ASSERT_EQ(data::ResultCode::Numeric::kNeedCompletion, client.pacman->install({"pull", target_to_install_json}).result_code.num_code);
    ASSERT_TRUE(boost::filesystem::exists(client.getRebootSentinel()));
    ASSERT_TRUE(boost::filesystem::exists(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::NeedStartFile));

    ASSERT_EQ("up --remove-orphans --no-start", Utils::readFile(client.tempdir->Path() / "apps/app1/up.log", true));

    currently_installed_hash = sha;
    client.fakeReboot();
    // make sure App has been restarted after reboot
    ASSERT_EQ("start", Utils::readFile(client.tempdir->Path() / "apps/app1/start.log", true));
    ASSERT_FALSE(boost::filesystem::exists(client.tempdir->Path() / "apps/app1" / Docker::ComposeApp::NeedStartFile));
  }
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
