#include <gtest/gtest.h>

#include "helpers.h"
#include "composeappmanager.h"
#include "primary/reportqueue.h"
#include "storage/invstorage.h"
#include "target.h"
#include "appengine.h"

#include "fixtures/basehttpclient.cc"

static boost::filesystem::path test_sysroot;


class DockerHttpClientMock: public fixtures::BaseHttpClient {
  public:
    HttpResponse get(const std::string &url, int64_t maxsize) override {
      if (url == "http://localhost/version") {
        return HttpResponse("{\"Arch\": \"amd64\"}", 200, CURLE_OK, "");
      }
      return HttpResponse("", 500, CURLE_OK, "not supported");
    }
};

static std::shared_ptr<AppEngine> createAppEngine(Config& config) {
  return std::make_shared<Docker::ComposeAppEngine>(
      config.pacman.extra["compose_apps_root"], config.pacman.extra["docker_compose_bin"],
      std::make_shared<Docker::DockerClient>(std::make_shared<DockerHttpClientMock>()), nullptr);
}

TEST(version, bad_versions) {
  ASSERT_TRUE(Target::Version("bar") < Target::Version("foo"));
  ASSERT_TRUE(Target::Version("1.bar") < Target::Version("2foo"));
  ASSERT_TRUE(Target::Version("1..0") < Target::Version("1.1"));
  ASSERT_TRUE(Target::Version("1.-1") < Target::Version("1.1"));
  ASSERT_TRUE(Target::Version("1.*bad #text") < Target::Version("1.1"));  // ord('*') < ord('1')
}

TEST(version, good_versions) {
  ASSERT_TRUE(Target::Version("1.0.1") < Target::Version("1.0.1.1"));
  ASSERT_TRUE(Target::Version("1.0.1") < Target::Version("1.0.2"));
  ASSERT_TRUE(Target::Version("0.9") < Target::Version("1.0.1"));
  ASSERT_TRUE(Target::Version("1.0.0.0") < Target::Version("1.0.0.1"));
  ASSERT_TRUE(Target::Version("1") < Target::Version("1.0.0.1"));
  ASSERT_TRUE(Target::Version("1.9.0") < Target::Version("1.10"));
}

// Ensure we finalize an install if completed
TEST(helpers, lite_client_finalize) {
  TemporaryDirectory cfg_dir;

  Config config;
  config.storage.path = cfg_dir.Path();
  config.pacman.type = ComposeAppManager::Name;
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.extra["docker_bin"] = "tests/docker_fake.sh";
  config.pacman.os = "dummy-os";
  config.pacman.booted = BootedType::kStaged;
  config.pacman.extra["compose_apps_root"] = (cfg_dir.Path() / "compose_apps").string();
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
  ASSERT_TRUE(target.MatchHash(LiteClient(config, createAppEngine(config)).getCurrent().hashes()[0]));

  config = Config();  // Create a new config since LiteClient std::move's it
  config.storage.path = cfg_dir.Path();
  config.pacman.type = ComposeAppManager::Name;
  config.pacman.sysroot = test_sysroot;
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.extra["docker_bin"] = "tests/docker_fake.sh";
  config.pacman.os = "dummy-os";
  config.pacman.booted = BootedType::kStaged;
  config.pacman.extra["compose_apps_root"] = (cfg_dir.Path() / "compose_apps").string();
  config.pacman.extra["compose_apps_tree"] = (cfg_dir.Path() / "apps-tree").string();
  config.pacman.extra["docker_images_reload_cmd"] = "/bin/true";

  target_json["hashes"]["sha256"] = "abcd";
  Uptane::Target new_target("test-finalize", target_json);
  storage->savePrimaryInstalledVersion(target, InstalledVersionUpdateMode::kPending);
  ASSERT_FALSE(new_target.MatchHash(LiteClient(config, createAppEngine(config)).getCurrent().hashes()[0]));
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

TEST(helpers, callback) {
  TemporaryDirectory cfg_dir;

  // First - invalid callback. We should detect and not crash
  Config bad_config;
  bad_config.bootloader.reboot_sentinel_dir = cfg_dir.Path();
  bad_config.pacman.sysroot = test_sysroot;
  bad_config.pacman.booted = BootedType::kStaged;
  bad_config.pacman.os = "dummy-os";
  bad_config.pacman.extra["compose_apps_root"] = (cfg_dir.Path() / "compose_apps").string();
  bad_config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  bad_config.pacman.extra["docker_bin"] = "tests/docker_fake.sh";
  bad_config.pacman.type = ComposeAppManager::Name;
  bad_config.storage.path = cfg_dir.Path();
  bad_config.pacman.extra["callback_program"] = "This does not exist";

  LiteClient bad_client(bad_config, createAppEngine(bad_config));
  ASSERT_EQ(0, bad_client.callback_program.size());
  bad_client.callback("Just call to make sure it doesnt crash", Uptane::Target::Unknown(), "");

  // Second - good callback. Make sure it works as expected
  Config config;
  config.bootloader.reboot_sentinel_dir = cfg_dir.Path();
  config.pacman.sysroot = test_sysroot;
  config.pacman.booted = BootedType::kStaged;
  config.pacman.extra["compose_apps_root"] = (cfg_dir.Path() / "compose_apps").string();
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.extra["docker_bin"] = "tests/docker_fake.sh";
  config.pacman.os = "dummy-os";
  config.pacman.type = ComposeAppManager::Name;
  config.storage.path = cfg_dir.Path();

  std::string cb = (cfg_dir / "callback.sh").native();
  std::string env = (cfg_dir / "callback.log").native();
  config.pacman.extra["callback_program"] = cb;

  std::string script("#!/bin/sh -e\n");
  script += "env > " + env;
  Utils::writeFile(cb, script);
  chmod(cb.c_str(), S_IRWXU);

  LiteClient(config, createAppEngine(config)).callback("AmigaOsInstall", Uptane::Target::Unknown(), "OK");
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
  config.pacman.booted = BootedType::kStaged;
  config.bootloader.reboot_sentinel_dir = cfg_dir.Path();
  config.pacman.extra["docker_compose_bin"] = "tests/compose_fake.sh";
  config.pacman.extra["docker_bin"] = "tests/docker_fake.sh";
  config.pacman.extra["compose_apps_root"] = (cfg_dir.Path() / "compose_apps").string();
  config.pacman.extra["compose_apps_tree"] = (cfg_dir.Path() / "apps-tree").string();
  config.pacman.extra["docker_images_reload_cmd"] = "/bin/true";

  std::shared_ptr<INvStorage> storage = INvStorage::newStorage(config.storage);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "deadbeef";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;

  return LiteClient(config, createAppEngine(config));
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

TEST(helpers, rollback_versions) {
  TemporaryDirectory cfg_dir;
  std::map<std::string, std::string> apps_cfg;
  LiteClient client = createClient(cfg_dir, apps_cfg);

  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  ASSERT_EQ(known_but_not_installed_versions.size(), 0);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = "sha-01";
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target_01{"target-01", target_json};

  // new Target was installed but not applied/finalized, reboot is required
  // in this case we should have zero known_but_not_installed versions
  client.storage->savePrimaryInstalledVersion(target_01, InstalledVersionUpdateMode::kPending);
  ASSERT_EQ(known_but_not_installed_versions.size(), 0);

  // a device is successfully rebooted on the new Target, so we still have zero "known but not installed"
  client.storage->savePrimaryInstalledVersion(target_01, InstalledVersionUpdateMode::kCurrent);
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  ASSERT_EQ(known_but_not_installed_versions.size(), 0);


  target_json["hashes"]["sha256"] = "sha-02";
  Uptane::Target target_02{"target-02", target_json};

  // new Target was installed but not applied/finalized, reboot is required
  // in this case we should have zero known_but_not_installed versions
  ASSERT_FALSE(known_local_target(client, target_02, known_but_not_installed_versions));
  client.storage->savePrimaryInstalledVersion(target_02, InstalledVersionUpdateMode::kPending);
  ASSERT_EQ(known_but_not_installed_versions.size(), 0);

  // a device is successfully rebooted on the new Target, so we still have zero "known but not installed"
  client.storage->savePrimaryInstalledVersion(target_02, InstalledVersionUpdateMode::kCurrent);
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  ASSERT_EQ(known_but_not_installed_versions.size(), 0);
  ASSERT_FALSE(known_local_target(client, target_02, known_but_not_installed_versions));

  target_json["hashes"]["sha256"] = "sha-03";
  Uptane::Target target_03{"target-03", target_json};

  // new Target was installed but not applied/finalized, reboot is required
  // in this case we should have zero known_but_not_installed versions
  ASSERT_FALSE(known_local_target(client, target_03, known_but_not_installed_versions));
  client.storage->savePrimaryInstalledVersion(target_03, InstalledVersionUpdateMode::kPending);
  ASSERT_EQ(known_but_not_installed_versions.size(), 0);

  // rollback has happened
  client.storage->savePrimaryInstalledVersion(target_03, InstalledVersionUpdateMode::kNone);
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  ASSERT_EQ(known_but_not_installed_versions.size(), 1);
  ASSERT_EQ(known_but_not_installed_versions[0].filename(), "target-03");
  ASSERT_TRUE(known_local_target(client, target_03, known_but_not_installed_versions));

  boost::optional<Uptane::Target> current_version;
  client.storage->loadPrimaryInstalledVersions(&current_version, nullptr);
  ASSERT_TRUE(current_version);
  ASSERT_EQ(current_version->filename(), "target-02");

  target_json["hashes"]["sha256"] = "sha-04";
  Uptane::Target target_04{"target-04", target_json};

  // new Target
  ASSERT_FALSE(known_local_target(client, target_04, known_but_not_installed_versions));
  client.storage->savePrimaryInstalledVersion(target_04, InstalledVersionUpdateMode::kPending);
  ASSERT_EQ(known_but_not_installed_versions.size(), 1);

  // reboot
  client.storage->savePrimaryInstalledVersion(target_04, InstalledVersionUpdateMode::kCurrent);
  known_but_not_installed_versions.clear();
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  ASSERT_FALSE(known_local_target(client, target_04, known_but_not_installed_versions));
  ASSERT_EQ(known_but_not_installed_versions.size(), 1);

  client.storage->loadPrimaryInstalledVersions(&current_version, nullptr);
  ASSERT_TRUE(current_version);
  ASSERT_EQ(current_version->filename(), "target-04");

  // manual update to target-02
  ASSERT_FALSE(known_local_target(client, target_02, known_but_not_installed_versions));
  client.storage->savePrimaryInstalledVersion(target_02, InstalledVersionUpdateMode::kCurrent);

  // go back to daemon mode and try to install the latest which is target-04
  ASSERT_FALSE(known_local_target(client, target_04, known_but_not_installed_versions));
  client.storage->savePrimaryInstalledVersion(target_04, InstalledVersionUpdateMode::kPending);
  // reboot
  client.storage->savePrimaryInstalledVersion(target_04, InstalledVersionUpdateMode::kCurrent);
  known_but_not_installed_versions.clear();

  // make sure that there is only one "bad" version after all updates
  known_but_not_installed_versions.clear();
  get_known_but_not_installed_versions(client, known_but_not_installed_versions);
  ASSERT_EQ(known_but_not_installed_versions.size(), 1);
  ASSERT_TRUE(known_local_target(client, target_03, known_but_not_installed_versions));
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
