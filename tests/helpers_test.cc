#include <gtest/gtest.h>

#include "helpers.h"
#include "composeappmanager.h"

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
  config.pacman.type = ComposeAppManager::Name;
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.os = "dummy-os";
  config.pacman.extra["booted"] = "0";
  config.pacman.extra["compose_apps_tree"] = (cfg_dir.Path() / "apps-tree").string();
  config.pacman.extra["docker_images_reload_cmd"] = "/bin/true";
  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  std::string sha = Utils::readFile(test_sysroot / "ostree/repo/refs/heads/ostree/1/1/0", true);
  Json::Value target_json;
  target_json["hashes"]["sha256"] = sha;
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target("test-finalize", target_json);

  storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
  ASSERT_TRUE(target.MatchHash(LiteClient(config).getCurrent().hashes()[0]));

  config = Config();  // Create a new config since LiteClient std::move's it
  config.storage.path = cfg_dir.Path();
  config.pacman.type = ComposeAppManager::Name;
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.os = "dummy-os";
  config.pacman.extra["booted"] = "0";
  config.pacman.extra["compose_apps_tree"] = (cfg_dir.Path() / "apps-tree").string();
  config.pacman.extra["docker_images_reload_cmd"] = "/bin/true";

  target_json["hashes"]["sha256"] = "abcd";
  Uptane::Target new_target("test-finalize", target_json);
  storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
  ASSERT_FALSE(new_target.MatchHash(LiteClient(config).getCurrent().hashes()[0]));
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
  auto target_json = Json::Value();
  target_json["length"] = 0;
  target_json["custom"]["targetFormat"] = "OSTREE";

  Uptane::Target t1{"target", target_json};
  Uptane::Target t2{"target", target_json};


  // t1 should equal t2 when there a no compose-apps
  ASSERT_TRUE(targets_eq(t1, t2, false));
  ASSERT_TRUE(targets_eq(t1, t2, true));

  auto custom = t1.custom_data();
  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v1";
  t1.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, false));  // still equal, ignoring compose-apps
  ASSERT_FALSE(targets_eq(t1, t2, true));

  custom = t2.custom_data();
  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v1";
  t2.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, true));

  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v2";
  t2.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // version has changed

  // Get things the same again
  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v1";
  t2.updateCustom(custom);

  custom["docker_compose_apps"]["app2"]["uri"] = "app2-v2";
  t2.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // t2 has an app that t1 doesn't

  custom = t1.custom_data();
  custom["docker_compose_apps"]["app2"]["uri"] = "app2-v1";
  t1.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // app2 versions differ

  custom["docker_compose_apps"]["app2"]["uri"] = "app2-v2";
  t1.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, true));
}

TEST(helpers, targets_eq_compose) {
  auto target_json = Json::Value();
  target_json["length"] = 0;
  target_json["custom"]["targetFormat"] = "OSTREE";

  Uptane::Target t1{"target", target_json};
  Uptane::Target t2{"target", target_json};

  // t1 should equal t2 when there a no compose-apps
  ASSERT_TRUE(targets_eq(t1, t2, false));
  ASSERT_TRUE(targets_eq(t1, t2, true));

  auto custom = t1.custom_data();
  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v1";
  t1.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, false));  // still equal, ignoring compose-apps
  ASSERT_FALSE(targets_eq(t1, t2, true));

  custom = t2.custom_data();
  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v1";
  t2.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, true));

  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v2";
  t2.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // version has changed

  // Get things the same again
  custom["docker_compose_apps"]["app1"]["uri"] = "app1-v1";
  t2.updateCustom(custom);

  custom["docker_compose_apps"]["app2"]["uri"] = "app2-v2";
  t2.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // t2 has an app that t1 doesn't

  custom = t1.custom_data();
  custom["docker_compose_apps"]["app2"]["uri"] = "app2-v1";
  t1.updateCustom(custom);
  ASSERT_FALSE(targets_eq(t1, t2, true));  // app2 versions differ

  custom["docker_compose_apps"]["app2"]["uri"] = "app2-v2";
  t1.updateCustom(custom);
  ASSERT_TRUE(targets_eq(t1, t2, true));
}

TEST(helpers, locking) {
  TemporaryDirectory cfg_dir;
  Config config;
  config.storage.path = cfg_dir.Path();
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra["booted"] = "0";

  LiteClient client(config);
  client.update_lockfile = cfg_dir / "update_lock";

  // 1. Create a lock and hold in inside a thread for a small amount of time
  std::unique_ptr<Lock> lock = client.getUpdateLock();
  std::chrono::steady_clock::time_point begin = std::chrono::steady_clock::now();
  std::thread t([_ = std::move(lock)] {  // pass ownership of ptr into lambda
    std::this_thread::sleep_for(std::chrono::milliseconds{500});
  });

  // 2. Get the lock - this should take a short period of time while its blocked
  //    by the thread.
  ASSERT_NE(nullptr, client.getUpdateLock());

  // 3. - make sure some time has passed
  std::chrono::steady_clock::time_point end = std::chrono::steady_clock::now();
  ASSERT_GT(std::chrono::duration_cast<std::chrono::milliseconds>(end - begin).count(), 300);

  t.join();
}

TEST(helpers, callback) {
  TemporaryDirectory cfg_dir;

  // First - invalid callback. We should detect and not crash
  Config bad_config;
  bad_config.bootloader.reboot_sentinel_dir = cfg_dir.Path();
  bad_config.pacman.sysroot = test_sysroot;
  bad_config.pacman.extra["booted"] = "0";
  bad_config.storage.path = cfg_dir.Path();
  bad_config.pacman.extra["callback_program"] = "This does not exist";

  LiteClient bad_client(bad_config);
  ASSERT_EQ(0, bad_client.callback_program.size());
  bad_client.callback("Just call to make sure it doesnt crash", Uptane::Target::Unknown());

  // Second - good callback. Make sure it works as expected
  Config config;
  config.bootloader.reboot_sentinel_dir = cfg_dir.Path();
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra["booted"] = "0";
  config.storage.path = cfg_dir.Path();

  std::string cb = (cfg_dir / "callback.sh").native();
  std::string env = (cfg_dir / "callback.log").native();
  config.pacman.extra["callback_program"] = cb;

  std::string script("#!/bin/sh -e\n");
  script += "env > " + env;
  Utils::writeFile(cb, script);
  chmod(cb.c_str(), S_IRWXU);

  LiteClient(config).callback("AmigaOsInstall", Uptane::Target::Unknown(), "OK");
  std::string line;
  std::ifstream in(env);
  bool found_target = false, found_message = false, found_result = false;
  while (std::getline(in, line)) {
    if (line.rfind("CURRENT_TARGET=", 0) == 0) {
      ASSERT_EQ((cfg_dir / "current-target").string(), line.substr(15));
      found_target = true;
    } else if (line.rfind("MESSAGE=", 0) == 0) {
      ASSERT_EQ("AmigaOsInstall", line.substr(8));
      found_message = true;
    } else if (line.rfind("RESULT=", 0) == 0) {
      ASSERT_EQ("OK", line.substr(7));
      found_result = true;
    }
  }
  ASSERT_TRUE(found_target);
  ASSERT_TRUE(found_message);
  ASSERT_TRUE(found_result);
}

static LiteClient createClient(TemporaryDirectory& cfg_dir,
                               std::map<std::string, std::string> extra,
                               std::string pacman_type = ComposeAppManager::Name) {
  Config config;
  config.storage.path = cfg_dir.Path();
  config.pacman.type = pacman_type;
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra = extra;
  config.pacman.extra["booted"] = "0";
  config.bootloader.reboot_sentinel_dir = cfg_dir.Path();
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.extra["compose_apps_tree"] = (cfg_dir.Path() / "apps-tree").string();
  config.pacman.extra["docker_images_reload_cmd"] = "/bin/true";

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "deadbeef";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  return LiteClient(config);
}

// Ensure we handle config changes of containers at start-up properly
TEST(helpers, containers_initialize) {
  TemporaryDirectory cfg_dir;

  auto apps_root = cfg_dir / "compose_apps";
  std::map<std::string, std::string> apps_cfg;
  apps_cfg["compose_apps_root"] = apps_root.native();

  // std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "deadbeef";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target("test-finalize", target_json);

  // Nothing different - all empty
  ASSERT_FALSE(createClient(cfg_dir, apps_cfg, ComposeAppManager::Name).composeAppsChanged());

  // Add a new app
  apps_cfg["compose_apps"] = "app1";

  ASSERT_TRUE(createClient(cfg_dir, apps_cfg, ComposeAppManager::Name).composeAppsChanged());

  // No apps configured, but one installed:
  apps_cfg["compose_apps"] = "";
  boost::filesystem::create_directories(apps_root / "app1");
  ASSERT_TRUE(createClient(cfg_dir, apps_cfg, ComposeAppManager::Name).composeAppsChanged());

  // One app configured, one app deployed
  apps_cfg["compose_apps"] = "app1";
  boost::filesystem::create_directories(apps_root / "app1");
  ASSERT_FALSE(createClient(cfg_dir, apps_cfg, ComposeAppManager::Name).composeAppsChanged());

  // Store the hash of the file and make sure no change is detected
  auto client = createClient(cfg_dir, apps_cfg, ComposeAppManager::Name);
  ASSERT_FALSE(client.composeAppsChanged());
}

TEST(helpers, compose_containers_initialize) {
  TemporaryDirectory cfg_dir;

  auto apps_root = cfg_dir / "compose_apps";
  std::map<std::string, std::string> apps_cfg;
  apps_cfg["compose_apps_root"] = apps_root.native();

  // std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "deadbeef";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target("test-finalize", target_json);

  // Nothing different - all empty
  ASSERT_FALSE(createClient(cfg_dir, apps_cfg).composeAppsChanged());

  // Add a new app
  apps_cfg["compose_apps"] = "app1";

  ASSERT_TRUE(createClient(cfg_dir, apps_cfg).composeAppsChanged());

  // No apps configured, but one installed:
  apps_cfg["compose_apps"] = "";
  boost::filesystem::create_directories(apps_root / "app1");
  ASSERT_TRUE(createClient(cfg_dir, apps_cfg).composeAppsChanged());

  // One app configured, one app deployed
  apps_cfg["compose_apps"] = "app1";
  boost::filesystem::create_directories(apps_root / "app1");
  ASSERT_FALSE(createClient(cfg_dir, apps_cfg).composeAppsChanged());
}

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
