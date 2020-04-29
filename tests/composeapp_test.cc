#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>

#include "http/httpclient.h"
#include "test_utils.h"

#include "composeappmanager.h"

static boost::filesystem::path test_sysroot;

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
  TestClient(const char* apps) {
    tempdir = std_::make_unique<TemporaryDirectory>();

    Config config;
    config.pacman.type = PACKAGE_MANAGER_COMPOSEAPP;
    config.pacman.sysroot = test_sysroot.string();
    config.pacman.extra["compose_apps_root"] = (*tempdir / "apps").native();
    config.pacman.extra["compose_apps"] = apps;
    config.pacman.extra["docker_compose_bin"] = "src/compose_fake.sh";
    config.storage.path = tempdir->Path();

    storage = INvStorage::newStorage(config.storage);
    pacman = std_::make_unique<ComposeAppManager>(config.pacman, config.bootloader, storage, nullptr);
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
  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  target_json["custom"]["docker_compose_apps"]["app1"]["uri"] = "n/a";
  target_json["custom"]["docker_compose_apps"]["app2"]["uri"] = "N/A";
  Uptane::Target target("pull", target_json);

  TestClient client("app2 doesnotexist");  // only app2 can be fetched
  bool result = client.pacman->fetchTarget(target, *(client.fetcher), *(client.keys), progress_cb, nullptr);
  ASSERT_TRUE(result);
  std::string output = Utils::readFile(client.tempdir->Path() / "apps/app2/download.log", true);
  ASSERT_EQ("download N/A", output);
  output = Utils::readFile(client.tempdir->Path() / "apps/app2/config.log", true);
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
