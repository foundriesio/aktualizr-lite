#include <gtest/gtest.h>

#include "helpers.h"

static boost::filesystem::path test_sysroot;

TEST(version, bad_versions) {
  ASSERT_TRUE(Version("bar") < Version("foo"));
  ASSERT_TRUE(Version("1.bar") < Version("2foo"));
  ASSERT_TRUE(Version("1..0") < Version("1.1"));
  ASSERT_TRUE(Version("1.-1") < Version("1.1"));
  ASSERT_TRUE(Version("1.*bad #text") < Version("1.1"));  // ord('*') < ord('1')
}

TEST(version, good_versions) {
  ASSERT_TRUE(Version("1.0.1") < Version("1.0.1.1"));
  ASSERT_TRUE(Version("1.0.1") < Version("1.0.2"));
  ASSERT_TRUE(Version("0.9") < Version("1.0.1"));
  ASSERT_TRUE(Version("1.0.0.0") < Version("1.0.0.1"));
  ASSERT_TRUE(Version("1") < Version("1.0.0.1"));
  ASSERT_TRUE(Version("1.9.0") < Version("1.10"));
}

// Ensure we finalize an install if completed
TEST(helpers, lite_client_finalize) {
  TemporaryDirectory cfg_dir;

  Config config;
  config.storage.path = cfg_dir.Path();
  config.pacman.type = PackageManager::kOstree;
  config.pacman.sysroot = test_sysroot;
  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "deadbeef";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target("test-finalize", target_json);

  setenv("OSTREE_HASH", "deadbeef", 1);
  storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
  ASSERT_TRUE(target.MatchHash(LiteClient(config).primary->getCurrent().hashes()[0]));

  config = Config();  // Create a new config since LiteClient std::move's it
  config.storage.path = cfg_dir.Path();
  config.pacman.type = PackageManager::kOstree;
  config.pacman.sysroot = test_sysroot;

  setenv("OSTREE_HASH", "abcd", 1);
  storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
  ASSERT_FALSE(target.MatchHash(LiteClient(config).primary->getCurrent().hashes()[0]));
}

TEST(helpers, target_has_tags) {
  auto t = Uptane::Target::Unknown();

  // No tags defined in target:
  std::vector<std::string> config_tags;
  ASSERT_TRUE(target_has_tags(t, config_tags));
  config_tags.push_back("foo");
  ASSERT_FALSE(target_has_tags(t, config_tags));

  // Set target tags to: premerge, qa
  auto custom = t.custom_data();
  custom["tags"].append("premerge");
  custom["tags"].append("qa");
  t.updateCustom(custom);

  config_tags.clear();
  ASSERT_TRUE(target_has_tags(t, config_tags));

  config_tags.push_back("qa");
  config_tags.push_back("blah");
  ASSERT_TRUE(target_has_tags(t, config_tags));

  config_tags.clear();
  config_tags.push_back("premerge");
  ASSERT_TRUE(target_has_tags(t, config_tags));

  config_tags.clear();
  config_tags.push_back("foo");
  ASSERT_FALSE(target_has_tags(t, config_tags));
}

TEST(helpers, targets_eq) {
  auto t1 = Uptane::Target::Unknown();
  auto t2 = Uptane::Target::Unknown();

  // t1 should equal t2 when there a no docker-apps
  ASSERT_TRUE(targets_eq(t1, t2, false));
  ASSERT_TRUE(targets_eq(t1, t2, true));

  auto custom = t1.custom_data();
  custom["docker_apps"]["app1"]["filename"] = "app1-v1";
  t1.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, false));  // still equal, ignoring docker-apps
  ASSERT_FALSE(targets_eq(t1, t2, true));

  custom = t2.custom_data();
  custom["docker_apps"]["app1"]["filename"] = "app1-v1";
  t2.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, true));

  custom["docker_apps"]["app1"]["filename"] = "app1-v2";
  t2.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // version has changed

  // Get things the same again
  custom["docker_apps"]["app1"]["filename"] = "app1-v1";
  t2.updateCustom(custom);

  custom["docker_apps"]["app2"]["filename"] = "app2-v2";
  t2.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // t2 has an app that t1 doesn't

  custom = t1.custom_data();
  custom["docker_apps"]["app2"]["filename"] = "app2-v1";
  t1.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // app2 versions differ

  custom["docker_apps"]["app2"]["filename"] = "app2-v2";
  t1.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, true));
}

TEST(helpers, locking) {
  TemporaryDirectory cfg_dir;
  Config config;
  config.storage.path = cfg_dir.Path();
  config.pacman.sysroot = test_sysroot;
  LiteClient client(config);
  client.update_lockfile = cfg_dir / "update_lock";

  // 1. Create a lock and hold in inside a thread for a small amount of time
  std::unique_ptr<Lock> lock = client.getUpdateLock();
  std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
  std::thread t([&lock] {
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
    lock->release();
  });

  // 2. Get the lock - this should take a short period of time while its blocked
  //    by the thread.
  ASSERT_NE(nullptr, client.getUpdateLock());

  // 3. - make sure some time has passed
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  ASSERT_GT(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count(), 300);

  t.join();
}

#ifdef BUILD_DOCKERAPP
// Ensure we handle config changes of containers at start-up properly
TEST(helpers, containers_initialize) {
  TemporaryDirectory cfg_dir;

  Config config;
  config.storage.path = cfg_dir.Path();
  config.pacman.type = PackageManager::kOstreeDockerApp;
  config.pacman.sysroot = test_sysroot;
  config.pacman.docker_apps_root = cfg_dir / "docker_apps";

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "deadbeef";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target("test-finalize", target_json);

  LiteClient client(config);

  // Nothing different - all empty
  ASSERT_FALSE(client.dockerAppsChanged());

  // Add a new app
  client.config.pacman.docker_apps.push_back("app1");
  ASSERT_TRUE(client.dockerAppsChanged());

  // No apps configured, but one installed:
  client.config.pacman.docker_apps.clear();
  boost::filesystem::create_directories(client.config.pacman.docker_apps_root / "app1");
  ASSERT_TRUE(client.dockerAppsChanged());

  // One app configured, one app deployed
  client.config.pacman.docker_apps.push_back("app1");
  boost::filesystem::create_directories(client.config.pacman.docker_apps_root / "app1");
  ASSERT_FALSE(client.dockerAppsChanged());

  // Docker app parameters enabled
  client.config.pacman.docker_app_params = cfg_dir / "foo.txt";
  Utils::writeFile(client.config.pacman.docker_app_params, std::string("foo text content"));
  ASSERT_TRUE(client.dockerAppsChanged());

  // Store the hash of the file and make sure no change is detected
  client.storeDockerParamsDigest();
  ASSERT_FALSE(client.dockerAppsChanged());

  // Change the content
  Utils::writeFile(client.config.pacman.docker_app_params, std::string("foo text content changed"));
  ASSERT_TRUE(client.dockerAppsChanged());

  // Disable and ensure we detect the change
  client.config.pacman.docker_app_params = "";
  ASSERT_TRUE(client.dockerAppsChanged());
  ASSERT_FALSE(boost::filesystem::exists(config.storage.path / ".params-hash"));
}
#endif

#ifndef __NO_MAIN__
int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  if (argc != 2) {
    std::cerr << "Error: " << argv[0] << " requires the path to an OSTree sysroot.\n";
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
