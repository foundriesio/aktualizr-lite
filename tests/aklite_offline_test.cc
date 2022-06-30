#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <fstream>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "appengine.h"
#include "composeappmanager.h"
#include "docker/restorableappengine.h"
#include "liteclient.h"
#include "target.h"

// include test fixtures
#include "fixtures/composeapp.cc"
#include "fixtures/dockerdaemon.cc"
#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"
#include "fixtures/liteclient/sysostreerepomock.cc"
#include "fixtures/liteclient/sysrootfs.cc"
#include "fixtures/liteclient/tufrepomock.cc"

class OfflineMetaFetcher : public Uptane::IMetadataFetcher {
 public:
  OfflineMetaFetcher(const boost::filesystem::path& tuf_repo_path) : tuf_repo_path_{tuf_repo_path / "repo" / "repo"} {}

  void fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo, const Uptane::Role& role,
                 Uptane::Version version) const override {
    const boost::filesystem::path meta_file_path{tuf_repo_path_ / version.RoleFileName(role)};
    if (!boost::filesystem::exists(meta_file_path)) {
      throw Uptane::MetadataFetchFailure(repo.ToString(), role.ToString());
    }
    std::ifstream meta_file_stream(meta_file_path.string());
    *result = {std::istreambuf_iterator<char>(meta_file_stream), std::istreambuf_iterator<char>()};
  }

  void fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                       const Uptane::Role& role) const override {
    fetchRole(result, maxsize, repo, role, Uptane::Version());
  }

 private:
  const boost::filesystem::path tuf_repo_path_;
};

class BaseHttpClient : public HttpInterface {
 public:
  HttpResponse post(const std::string&, const std::string&, const std::string&) override {
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse post(const std::string&, const Json::Value&) override { return HttpResponse("", 501, CURLE_OK, ""); }
  HttpResponse put(const std::string&, const std::string&, const std::string&) override {
    return HttpResponse("", 501, CURLE_OK, "");
  }
  HttpResponse put(const std::string&, const Json::Value&) override { return HttpResponse("", 501, CURLE_OK, ""); }
  HttpResponse download(const std::string& url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void* userp, curl_off_t from) override {
    return HttpResponse("", 501, CURLE_OK, "");
  }
  std::future<HttpResponse> downloadAsync(const std::string& url, curl_write_callback write_cb,
                                          curl_xferinfo_callback progress_cb, void* userp, curl_off_t from,
                                          CurlHandler* easyp) override {
    std::promise<HttpResponse> resp_promise;
    resp_promise.set_value(HttpResponse("", 501, CURLE_OK, ""));
    return resp_promise.get_future();
  }
  void setCerts(const std::string&, CryptoSource, const std::string&, CryptoSource, const std::string&,
                CryptoSource) override {}
};

class RegistryBasicAuthClient : public BaseHttpClient {
 public:
  HttpResponse get(const std::string& url, int64_t maxsize) override {
    return HttpResponse("{\"Secret\":\"secret\",\"Username\":\"test-user\"}", 200, CURLE_OK, "");
  }
};

class OfflineRegistry : public BaseHttpClient {
 public:
  OfflineRegistry(const boost::filesystem::path& root_dir, const std::string& hostname = "hub.foundries.io")
      : hostname_{hostname}, root_dir_{root_dir} {}

  HttpResponse get(const std::string& url, int64_t maxsize) override {
    if (boost::starts_with(url, auth_endpoint_)) {
      return HttpResponse("{\"token\":\"token\"}", 200, CURLE_OK, "");
    }
    return getAppItem(url);
  }

  HttpResponse download(const std::string& url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb,
                        void* userp, curl_off_t from) override {
    const std::string hash_prefix{"sha256:"};
    const auto digest_pos{url.rfind(hash_prefix)};
    if (digest_pos == std::string::npos) {
      return HttpResponse("Invalid URL", 400, CURLE_OK, "");
    }
    const auto hash_pos{digest_pos + hash_prefix.size()};
    const auto hash{url.substr(hash_pos)};

    char buf[1024 * 4];
    std::ifstream blob_file{(blobs_dir_ / hash).string()};

    std::streamsize read_byte_numb;
    while ((read_byte_numb = blob_file.readsome(buf, sizeof(buf))) > 0) {
      write_cb(buf, read_byte_numb, 1, userp);
    }
    blob_file.close();
    return HttpResponse("", 200, CURLE_OK, "");
  }

  HttpResponse getAppItem(const std::string& url) const {
    const std::string hash_prefix{"sha256:"};
    const auto digest_pos{url.rfind(hash_prefix)};
    if (digest_pos == std::string::npos) {
      return HttpResponse("Invalid URL", 400, CURLE_OK, "");
    }
    const auto hash_pos{digest_pos + hash_prefix.size()};
    const auto hash{url.substr(hash_pos)};
    return HttpResponse(Utils::readFile(blobs_dir_ / hash), 200, CURLE_OK, "");
  }

  AppEngine::App addApp(const fixtures::ComposeApp::Ptr& app) {
    const auto app_dir{apps_dir_ / app->name() / app->hash()};

    // store App data
    boost::filesystem::create_directories(app_dir);
    Utils::writeFile(app_dir / "manifest.json", app->manifest());
    Utils::writeFile(blobs_dir_ / app->hash(), app->manifest());
    Utils::writeFile(app_dir / (app->archHash() + ".tgz"), app->archive());
    Utils::writeFile(blobs_dir_ / app->archHash(), app->archive());
    Utils::writeFile(blobs_dir_ / app->layersHash(), app->layersManifest());

    // store image in the skopeo/OCI compliant way
    const auto image_uri{app->image().uri()};
    Docker::Uri uri{Docker::Uri::parseUri(image_uri)};
    const auto image_dir{app_dir / "images" / uri.registryHostname / uri.repo / uri.digest.hash()};
    boost::filesystem::create_directories(image_dir);
    Utils::writeFile(image_dir / "oci-layout", std::string("{\"imageLayoutVersion\": \"1.0.0\"}"));

    Json::Value index_json;
    index_json["schemaVersion"] = 2;
    index_json["manifests"][0]["mediaType"] = "application/vnd.docker.distribution.manifest.v2+json";
    index_json["manifests"][0]["digest"] = "sha256:" + app->image().manifest().hash;
    index_json["manifests"][0]["size"] = app->image().manifest().size;
    Utils::writeFile(image_dir / "index.json", Utils::jsonToStr(index_json));
    Utils::writeFile(blobs_dir_ / app->image().manifest().hash, app->image().manifest().data);
    Utils::writeFile(blobs_dir_ / app->image().config().hash, app->image().config().data);
    Utils::writeFile(blobs_dir_ / app->image().layerBlob().hash, app->image().layerBlob().data);

    return {app->name(), hostname_ + '/' + "factory/" + app->name() + '@' + "sha256:" + app->hash()};
  }

  boost::filesystem::path blobsDir() const { return root_dir_ / "blobs"; }
  const boost::filesystem::path& appsDir() const { return apps_dir_; }

 private:
  const boost::filesystem::path root_dir_;
  const std::string hostname_;
  const std::string auth_endpoint_{"https://" + hostname_ + "/token-auth"};
  const boost::filesystem::path apps_dir_{root_dir_ / "apps"};
  const boost::filesystem::path blobs_dir_{root_dir_ / "blobs" / "sha256"};
};

class AkliteOffline : public ::testing::Test {
 protected:
  AkliteOffline()
      : sys_rootfs_{(test_dir_.Path() / "sysroot-fs").string(), branch, hw_id, os},
        sys_repo_{(test_dir_.Path() / "sysrepo").string(), os},
        ostree_repo_{(test_dir_.Path() / "treehub").string(), true},
        tuf_repo_{test_dir_.Path() / "tuf"},
        meta_fetcher_{new OfflineMetaFetcher(tuf_repo_.getPath())},
        daemon_{test_dir_.Path() / "daemon"} {
    // a path to the config dir
    cfg_.storage.path = test_dir_.Path() / "sota-dir";

    // ostree-based sysroot config
    cfg_.pacman.sysroot = sys_repo_.getPath();
    cfg_.pacman.os = os;
    cfg_.pacman.booted = BootedType::kStaged;

    // configure bootloader/booting related functionality
    cfg_.bootloader.reboot_command = "/bin/true";
    cfg_.bootloader.reboot_sentinel_dir = test_dir_.Path();

    // add an initial rootfs to the ostree-based sysroot that liteclient manages and deploy it
    const auto hash = sys_repo_.getRepo().commit(sys_rootfs_.path, sys_rootfs_.branch);
    sys_repo_.deploy(hash);
  }

  Uptane::Target addTarget(const std::vector<AppEngine::App>* apps = nullptr) {
    const auto& latest_target{tuf_repo_.getLatest()};
    std::string version;
    if (version.size() == 0) {
      try {
        version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
      } catch (...) {
        LOG_INFO << "No target available, preparing the first version";
        version = "1";
      }
    }

    // update rootfs and commit it into Treehub's repo
    const std::string unique_content = Utils::randomUuid();
    const std::string unique_file = Utils::randomUuid();
    Utils::writeFile(sys_rootfs_.path + "/" + unique_file, unique_content, true);
    auto hash = ostree_repo_.commit(sys_rootfs_.path, os);

    Json::Value apps_json;
    if (apps) {
      for (const auto& app : *apps) {
        apps_json[app.name]["uri"] = app.uri;
      }
    }

    // add new target to TUF repo
    const std::string name = hw_id + "-" + os + "-" + version;
    return tuf_repo_.addTarget(name, hash, hw_id, version, apps_json);
  }

  const std::string getSentinelFilePath() const {
    return (cfg_.bootloader.reboot_sentinel_dir / "need_reboot").string();
  }

  void reboot() { boost::filesystem::remove(getSentinelFilePath()); }

 protected:
  static const std::string branch;
  static const std::string hw_id;
  static const std::string os;

 protected:
  TemporaryDirectory test_dir_;  // must be the first element in the class
  Config cfg_;
  SysRootFS sys_rootfs_;        // a sysroot that is bitbaked and added to the ostree repo that liteclient fetches from
  SysOSTreeRepoMock sys_repo_;  // an ostree-based sysroot that liteclient manages
  OSTreeRepoMock ostree_repo_;  // a source ostree repo to fetch update from
  TufRepoMock tuf_repo_;
  fixtures::DockerDaemon daemon_;

  std::shared_ptr<Uptane::IMetadataFetcher> meta_fetcher_;
};

const std::string AkliteOffline::branch{"lmp"};
const std::string AkliteOffline::hw_id{"raspberrypi4-64"};
const std::string AkliteOffline::os{"lmp"};

TEST_F(AkliteOffline, FetchMeta) {
  LiteClient client(cfg_, nullptr, nullptr, meta_fetcher_);
  const auto target{addTarget()};

  ASSERT_TRUE(client.checkForUpdatesBegin());
  const auto targets{client.allTargets()};
  ASSERT_GE(targets.size(), 1);
  ASSERT_EQ(targets[0].filename(), target.filename());
  ASSERT_NO_THROW(client.checkForUpdatesEnd(targets[0]));
}

TEST_F(AkliteOffline, FetchAndInstallOstree) {
  // use the ostree package manager to avoid fetching Apps in this test
  cfg_.pacman.type = "ostree";
  // instruct the lite client (ostree pac manager) to fetch an ostree commit from a local repo
  cfg_.pacman.ostree_server = "file://" + ostree_repo_.getPath();

  Uptane::Target target{Uptane::Target::Unknown()};
  {
    // somehow liteclient/aktualizr modifies an input config so we have to make a copy of it
    // to avoid modification of the original configuration.
    Config cfg{cfg_};
    LiteClient client(cfg, nullptr, nullptr, meta_fetcher_);
    target = addTarget();

    // pull TUF metadata
    ASSERT_TRUE(client.checkForUpdatesBegin());
    const auto targets{client.allTargets()};
    ASSERT_GE(targets.size(), 1);
    ASSERT_EQ(targets[0].filename(), target.filename());
    ASSERT_NO_THROW(client.checkForUpdatesEnd(targets[0]));

    // pull and install an ostree commit that Target refers to
    ASSERT_TRUE(client.download(target, ""));
    ASSERT_EQ(client.install(target), data::ResultCode::Numeric::kNeedCompletion);
  }

  reboot();

  {
    Config cfg{cfg_};
    LiteClient client(cfg, nullptr, nullptr, meta_fetcher_);
    ASSERT_TRUE(client.finalizeInstall());
    ASSERT_EQ(client.getCurrent().filename(), target.filename());
    ASSERT_EQ(client.getCurrent().sha256Hash(), target.sha256Hash());
  }
}

TEST_F(AkliteOffline, FetchAndInstallApps) {
  boost::filesystem::create_directories(test_dir_.Path() / "compose-bin-dir");
  cfg_.pacman.type = "ostree+compose_apps";
  cfg_.pacman.extra["docker_compose_bin"] =
      boost::filesystem::canonical("tests/docker-compose_fake.py").string() + " " + daemon_.dir().string() + " ";
  cfg_.pacman.extra["compose_apps_root"] = (test_dir_.Path() / "compose-apps").string();
  cfg_.pacman.extra["reset_apps"] = "";
  cfg_.pacman.extra["reset_apps_root"] = (test_dir_.Path() / "reset-apps").string();
  // instruct the lite client (ostree pac manager) to fetch an ostree commit from a local repo
  cfg_.pacman.ostree_server = "file://" + ostree_repo_.getPath();

  ComposeAppManager::Config pacman_cfg(cfg_.pacman);

  const boost::filesystem::path store_root{pacman_cfg.reset_apps_root};
  const boost::filesystem::path install_root{pacman_cfg.apps_root};
  const boost::filesystem::path docker_root{pacman_cfg.images_data_root};
  std::shared_ptr<HttpInterface> registry_basic_auth_client{std::make_shared<RegistryBasicAuthClient>()};
  std::shared_ptr<OfflineRegistry> offline_registry{std::make_shared<OfflineRegistry>(test_dir_.Path() / "registry")};

  Docker::RegistryClient::Ptr registry_client{std::make_shared<Docker::RegistryClient>(
      registry_basic_auth_client, "foobar",
      [&offline_registry](const std::vector<std::string>*) { return offline_registry; })};

  AppEngine::Ptr app_engine{std::make_shared<Docker::RestorableAppEngine>(
      store_root, install_root, docker_root, registry_client,
      std::make_shared<Docker::DockerClient>(daemon_.getClient()), pacman_cfg.skopeo_bin.string(), daemon_.getUrl(),
      pacman_cfg.compose_bin.string(), Docker::RestorableAppEngine::GetDefStorageSpaceFunc(),
      [&offline_registry](const Docker::Uri& app_uri, const std::string& image_uri) {
        Docker::Uri uri{Docker::Uri::parseUri(image_uri)};
        return "--src-shared-blob-dir " + offline_registry->blobsDir().string() +
               " oci:" + offline_registry->appsDir().string() + "/" + app_uri.app + "/" + app_uri.digest.hash() +
               "/images/" + uri.registryHostname + "/" + uri.repo + "/" + uri.digest.hash();
      },
      false /* don't create containers on install because it makes dockerd to check if pinned images
    present in its store what we should avoid until images are registered (hacked) in dockerd store
  */)};

  Uptane::Target target{Uptane::Target::Unknown()};

  const auto layer_size{1024};
  Json::Value layers;
  layers["layers"][0]["digest"] =
      "sha256:" + boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(Utils::randomUuid())));
  layers["layers"][0]["size"] = layer_size;

  const auto app{offline_registry->addApp(fixtures::ComposeApp::createAppWithCustomeLayers("app-01", layers))};
  const std::vector<AppEngine::App> apps{app};
  target = addTarget(&apps);

  {
    Config cfg{cfg_};
    LiteClient client(cfg, app_engine, nullptr, meta_fetcher_);

    ASSERT_TRUE(client.checkForUpdatesBegin());
    const auto targets{client.allTargets()};
    ASSERT_GE(targets.size(), 1);
    ASSERT_EQ(targets[0].filename(), target.filename());
    ASSERT_NO_THROW(client.checkForUpdatesEnd(targets[0]));
    ASSERT_TRUE(client.download(target, ""));
    ASSERT_EQ(client.install(target), data::ResultCode::Numeric::kNeedCompletion);
    ASSERT_FALSE(app_engine->isRunning(app));
  }

  reboot();

  {
    Config cfg{cfg_};
    LiteClient client(cfg, app_engine, nullptr, meta_fetcher_);
    ASSERT_TRUE(client.finalizeInstall());
    ASSERT_EQ(client.getCurrent().filename(), target.filename());
    ASSERT_EQ(client.getCurrent().sha256Hash(), target.sha256Hash());
    ASSERT_TRUE(app_engine->isRunning(app));
  }
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  SysRootFS::CreateCmd = argv[1];
  return RUN_ALL_TESTS();
}
