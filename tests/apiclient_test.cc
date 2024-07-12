#include <gmock/gmock.h>
#include <gtest/gtest.h>

#include <boost/filesystem.hpp>
#include <boost/process.hpp>
#include <boost/process/env.hpp>

#include "libaktualizr/types.h"
#include "logging/logging.h"
#include "test_utils.h"
#include "uptane_generator/image_repo.h"
#include "utilities/utils.h"

#include "aktualizr-lite/aklite_client_ext.h"
#include "aktualizr-lite/api.h"
#include "composeappmanager.h"
#include "liteclient.h"

#include "docker/composeappengine.h"
#include "helpers.h"
#include "ostree/repo.h"
#include "target.h"

#include "fixtures/liteclienttest.cc"

#include <gmock/gmock.h>
#include <gtest/gtest.h>

using ::testing::NiceMock;

class ApiClientTest : public fixtures::ClientTest {
 protected:
  std::shared_ptr<fixtures::LiteClientMock> createLiteClient(
      InitialVersion initial_version = InitialVersion::kOn,
      boost::optional<std::vector<std::string>> apps = boost::none, bool finalize = true) override {
    app_engine_mock_ = std::make_shared<NiceMock<fixtures::MockAppEngine>>();
    lite_client_ = ClientTest::createLiteClient(app_engine_mock_, initial_version, apps);
    return lite_client_;
  }

  std::shared_ptr<NiceMock<fixtures::MockAppEngine>>& getAppEngine() { return app_engine_mock_; }
  bool resetEvents() { return getDeviceGateway().resetEvents(lite_client_->http_client); }
  void tweakConf(Config& conf) override {
    if (!pacman_type_.empty()) {
      conf.pacman.type = pacman_type_;
    }
  }

  void setPacmanType(const std::string& pacman_type) { pacman_type_ = pacman_type; }

  std::string addOstreeCommit() {
    const std::string unique_content = Utils::randomUuid();
    const std::string unique_file = Utils::randomUuid();
    Utils::writeFile(getSysRootFs().path + "/" + unique_file, unique_content, true);
    return getOsTreeRepo().commit(getSysRootFs().path, "lmp");
  }

 private:
  std::shared_ptr<NiceMock<fixtures::MockAppEngine>> app_engine_mock_;
  std::shared_ptr<fixtures::LiteClientMock> lite_client_;
  std::string pacman_type_;
};

TEST_F(ApiClientTest, GetConfig) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));
  ASSERT_EQ("\"ostree+compose_apps\"", client.GetConfig().get("pacman.type", ""));
}

TEST_F(ApiClientTest, GetCurrent) {
  auto cur = AkliteClient(createLiteClient(InitialVersion::kOff)).GetCurrent();
  ASSERT_EQ(Target::InitialTarget, cur.Name());
  ASSERT_EQ(-1, cur.Version());
}

TEST_F(ApiClientTest, GetDevice) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));
  auto res = client.GetDevice();
  ASSERT_EQ(DeviceResult::Status::Ok, res.status);
  ASSERT_EQ("fake-device", res.name);
  ASSERT_EQ("fake-factory", res.factory);
  ASSERT_EQ("fake-owner", res.owner);
  ASSERT_EQ("fake-id", res.repo_id);
}

TEST_F(ApiClientTest, CheckIn) {
  auto lite_client = createLiteClient(InitialVersion::kOn);
  AkliteClient client(lite_client);
  EXPECT_CALL(*lite_client, callback(testing::StrEq("check-for-update-pre"), testing::_, testing::StrEq(""))).Times(1);
  EXPECT_CALL(*lite_client, callback(testing::StrEq("check-for-update-post"), testing::_, testing::StrEq("OK")));

  auto result = client.CheckIn();

  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ(2, events.size());
  auto val = getDeviceGateway().readSotaToml();
  ASSERT_NE(std::string::npos, val.find("[pacman]"));

  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(1, result.Targets().size());

  ASSERT_TRUE(getDeviceGateway().resetSotaToml());
  ASSERT_TRUE(resetEvents());

  auto new_target = createTarget();

  EXPECT_CALL(*lite_client, callback(testing::StrEq("check-for-update-pre"), testing::_, testing::StrEq(""))).Times(1);
  EXPECT_CALL(*lite_client, callback(testing::StrEq("check-for-update-post"), testing::_, testing::StrEq("OK")));
  result = client.CheckIn();
  ASSERT_EQ(0, getDeviceGateway().getEvents().size());
  ASSERT_EQ("", getDeviceGateway().readSotaToml());
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(2, result.Targets().size());
  ASSERT_EQ(new_target.filename(), result.Targets()[1].Name());
  ASSERT_EQ(new_target.sha256Hash(), result.Targets()[1].Sha256Hash());
}

TEST_F(ApiClientTest, CheckInLocal) {
  setPacmanType(RootfsTreeManager::Name);
  AkliteClient client(createLiteClient(InitialVersion::kOn));

  // Accessing repo metadata files directly from the local filesystem
  auto repo_dir = getTufRepo().getRepoPath();
  tuf_repo_.updateBundleMeta(initial_target_.filename());

  const LocalUpdateSource local_update_source_ = {repo_dir, ostree_repo_.getPath()};
  auto result = client.CheckInLocal(&local_update_source_);
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(1, result.Targets().size());

  // No communication is done with the device gateway inside CheckInLocal
  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ(0, events.size());
  ASSERT_EQ("", getDeviceGateway().readSotaToml());

  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(1, result.Targets().size());

  auto new_target = createTarget();
  tuf_repo_.updateBundleMeta(new_target.filename());
  result = client.CheckInLocal(&local_update_source_);
  ASSERT_EQ(0, getDeviceGateway().getEvents().size());
  ASSERT_EQ("", getDeviceGateway().readSotaToml());
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(2, result.Targets().size());
  ASSERT_EQ(new_target.filename(), result.GetLatest().Name());
  ASSERT_EQ(new_target.sha256Hash(), result.GetLatest().Sha256Hash());
}

TEST_F(ApiClientTest, CheckInWithoutTargetImport) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));

  auto result = client.CheckIn();

  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ(2, events.size());
  auto val = getDeviceGateway().readSotaToml();
  ASSERT_NE(std::string::npos, val.find("[pacman]"));

  ASSERT_EQ(CheckInResult::Status::NoMatchingTargets, result.status);
  ASSERT_EQ(0, result.Targets().size());

  ASSERT_TRUE(getDeviceGateway().resetSotaToml());
  ASSERT_TRUE(resetEvents());

  auto new_target = createTarget();
  result = client.CheckIn();
  ASSERT_EQ(0, getDeviceGateway().getEvents().size());
  ASSERT_EQ("", getDeviceGateway().readSotaToml());
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_EQ(1, result.Targets().size());
  ASSERT_EQ(new_target.filename(), result.Targets()[0].Name());
  ASSERT_EQ(new_target.sha256Hash(), result.Targets()[0].Sha256Hash());
}

TEST_F(ApiClientTest, Rollback) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*liteclient, getInitialTarget(), new_target);

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  ASSERT_FALSE(client.IsRollback(result.GetLatest()));

  // deploy the initial version/commit to emulate rollback
  getSysRepo().deploy(getInitialTarget().sha256Hash());

  reboot(liteclient);
  // reboot re-creates an instance of LiteClient so `client` refers to an invalid/removed instance of LiteClient now,
  // hence we need to re-create an instance of AkliteClient
  AkliteClient rebooted_client(liteclient);

  ASSERT_TRUE(rebooted_client.IsRollback(result.GetLatest()));
  ASSERT_EQ(rebooted_client.GetCurrent().Sha256Hash(), getInitialTarget().sha256Hash());
}

TEST_F(ApiClientTest, Install) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  auto latest = result.GetLatest();

  auto installer = client.Installer(latest);
  ASSERT_NE(nullptr, installer);
  auto dresult = installer->Download();
  ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

  auto iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);
}

TEST_F(ApiClientTest, InstallWithCorrelationId) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  auto latest = result.GetLatest();

  resetEvents();

  auto installer = client.Installer(latest, "", "this-is-random");
  ASSERT_NE(nullptr, installer);
  auto dresult = installer->Download();
  ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

  auto iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);

  ASSERT_EQ("this-is-random", installer->GetCorrelationId());
  // drain all events to DG by recreating the report queue instance
  liteclient->report_queue =
      std::make_unique<ReportQueue>(liteclient->config, liteclient->http_client, liteclient->storage, 0, 1);
  // wait a bit to make sure all events arrive to DG
  std::this_thread::sleep_for(std::chrono::milliseconds(50));
  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ("this-is-random", events[0]["event"]["correlationId"].asString());
}

TEST_F(ApiClientTest, InstallModeOstreeOnlyIfOstreeAndApps) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  std::vector<AppEngine::App> apps{{"app-01", "app-01-URI"}};
  auto new_target = createTarget(&apps);
  {
    AkliteClient client(liteclient);

    EXPECT_CALL(*getAppEngine(), fetch).Times(1);
    // make sure App install is not called
    EXPECT_CALL(*getAppEngine(), install).Times(0);

    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();
    auto installer = client.Installer(latest, "", "", InstallMode::OstreeOnly);
    ASSERT_NE(nullptr, installer);
    auto dresult = installer->Download();
    ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

    auto iresult = installer->Install();
    ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);
    reboot(liteclient);
  }
  {
    AkliteClient client(liteclient);

    auto ciresult = client.CompleteInstallation();
    ASSERT_EQ(InstallResult::Status::Ok, ciresult.status);
  }
}

TEST_F(ApiClientTest, InstallModeOstreeOnlyIfJustApps) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  std::vector<AppEngine::App> apps{{"app-01", "app-01-URI"}};
  auto new_target = createAppTarget(apps);
  AkliteClient client(liteclient);

  EXPECT_CALL(*getAppEngine(), fetch).Times(1);
  // make sure App install is not called
  EXPECT_CALL(*getAppEngine(), install).Times(0);

  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  auto latest = result.GetLatest();
  auto installer = client.Installer(latest, "", "", InstallMode::OstreeOnly);
  ASSERT_NE(nullptr, installer);
  auto dresult = installer->Download();
  ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

  auto iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::AppsNeedCompletion, iresult.status);

  {
    liteclient = createLiteClient();
    AkliteClient client(liteclient);
    auto ciresult = client.CompleteInstallation();
    ASSERT_EQ(InstallResult::Status::Ok, ciresult.status);
  }
}

TEST_F(ApiClientTest, InstallWithoutDownload) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();

  AkliteClient client(liteclient);
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  auto latest = result.GetLatest();

  auto installer = client.Installer(latest);
  ASSERT_NE(nullptr, installer);

  // Install before Download will fail
  auto iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::DownloadFailed, iresult.status);

  auto dresult = installer->Download();
  ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

  // After Download, installation of the same target should succeed
  iresult = installer->Install();
  ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);
}

TEST_F(ApiClientTest, InstallDownloadInSeparateInstances) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  std::vector<AppEngine::App> apps_1{{"app-01", "app-01-URI"}};
  auto target_1 = createAppTarget(apps_1);

  // Download using one AkliteClient instance
  {
    AkliteClient client(liteclient);
    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();
    auto installer = client.Installer(latest);
    ASSERT_NE(nullptr, installer);
    auto dresult = installer->Download();
    ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);
    ASSERT_FALSE(targetsMatch(liteclient->getCurrent(), target_1));
  }

  // Install using another AkliteClient instance
  {
    AkliteClient client(liteclient);
    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();
    auto installer = client.Installer(latest);
    ASSERT_NE(nullptr, installer);
    auto iresult = installer->Install();
    ASSERT_EQ(InstallResult::Status::Ok, iresult.status);
    ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), target_1));
  }

  // Repeat same process just updating one app
  std::vector<AppEngine::App> apps_2{{"app-01", "app-01-URI-NEW"}};
  auto target_2 = createAppTarget(apps_2);

  // Download using one AkliteClient instance
  {
    AkliteClient client(liteclient);
    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();
    auto installer = client.Installer(latest);
    ASSERT_NE(nullptr, installer);
    auto dresult = installer->Download();
    ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);
    ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), target_1));
  }

  // Install using another AkliteClient instance
  {
    AkliteClient client(liteclient);
    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();
    auto installer = client.Installer(latest);
    ASSERT_NE(nullptr, installer);
    auto iresult = installer->Install();
    ASSERT_EQ(InstallResult::Status::Ok, iresult.status);
    ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), target_2));
  }
}

TEST_F(ApiClientTest, Secondaries) {
  AkliteClient client(createLiteClient(InitialVersion::kOff));
  std::vector<SecondaryEcu> ecus;
  ecus.emplace_back("123", "riscv", "target12");
  auto res = client.SetSecondaries(ecus);
  ASSERT_EQ(InstallResult::Status::Ok, res.status);
  auto events = getDeviceGateway().getEvents();
  ASSERT_EQ(1, events.size());
  ASSERT_EQ("target12", events[0]["123"]["target"].asString());
  ASSERT_EQ("riscv", events[0]["123"]["hwid"].asString());

  auto new_target = createTarget();
  auto secondary_target = createTarget(nullptr, "riscv");
  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);

  ASSERT_EQ(2, result.Targets().size());
  ASSERT_EQ(new_target.filename(), result.GetLatest().Name());
  ASSERT_EQ(secondary_target.filename(), result.GetLatest("riscv").Name());
}

TEST_F(ApiClientTest, SwitchTag) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  const auto tagged_repo_path{test_dir_ / "tagged_repo"};
  // copy TUF repo
  Utils::copyDir(getTufRepo().getPath(), tagged_repo_path);
  fixtures::TufRepoMock tag_repo{tagged_repo_path, "", "corellation-id", false};
  tag_repo.setLatest(getTufRepo().getLatest());

  const auto master_target{createTarget()};
  const auto tag_target{createTarget(nullptr, "", "", tag_repo)};
  // now both repo have the same root.json but different timestamp, snapshot and targets metadata,
  // and their versions are the same (metadata's version = 3, Target custom version 2)

  {
    AkliteClient client(liteclient);
    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();

    auto installer = client.Installer(latest);
    ASSERT_NE(nullptr, installer);
    auto dresult = installer->Download();
    ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

    auto iresult = installer->Install();
    ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);
  }

  // reboot
  {
    reboot(liteclient);
    AkliteClient client(liteclient);

    // make sure the update to master_target was successful
    ASSERT_EQ(client.GetCurrent().Name(), master_target.filename());
    ASSERT_EQ(client.GetCurrent().Sha256Hash(), master_target.sha256Hash());
  }

  // switch tag and restart
  {
    // switch to the tag repo
    // for some reason Utils::copyDir fails time to time if a destination is not empty
    // even though it calls remove_all internally, so just remove by invoking a shell cmd
    std::string rm_out;
    ASSERT_EQ(Utils::shell("rm -rf " + getTufRepo().getPath(), &rm_out, true), 0) << rm_out;
    Utils::copyDir(tagged_repo_path, getTufRepo().getPath());

    restart(liteclient);
    AkliteClient client(liteclient);

    auto result = client.CheckIn();
    ASSERT_EQ(CheckInResult::Status::Ok, result.status);

    auto latest = result.GetLatest();
    // make sure the latest matches the latest from the tag repo, i.e. the tag target
    ASSERT_EQ(latest.Name(), tag_target.filename());
    ASSERT_EQ(latest.Sha256Hash(), tag_target.sha256Hash());
    // make sure that the current and latest versions are the same but their content is different
    ASSERT_EQ(latest.Version(), client.GetCurrent().Version());
    ASSERT_NE(latest.Sha256Hash(), client.GetCurrent().Sha256Hash());

    // do install
    auto installer = client.Installer(latest);
    // if metadata update was incorrect and currently stored metadata are not consistent then
    // this check fails because AkliteClient::Installer does check "offline"/stored metadata
    ASSERT_NE(nullptr, installer);
    auto dresult = installer->Download();
    ASSERT_EQ(DownloadResult::Status::Ok, dresult.status);

    auto iresult = installer->Install();
    ASSERT_EQ(InstallResult::Status::NeedsCompletion, iresult.status);
  }

  // reboot
  {
    reboot(liteclient);
    AkliteClient client(liteclient);

    // make sure the update to tag_target was successful
    ASSERT_EQ(client.GetCurrent().Name(), tag_target.filename());
    ASSERT_EQ(client.GetCurrent().Sha256Hash(), tag_target.sha256Hash());
  }
}

TEST_F(ApiClientTest, InstallTargetWithHackedOstree) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  std::vector<AppEngine::App> apps{{"app-01", "app-01-URI"}};
  auto valid_target = Target::toTufTarget(createTarget(&apps));
  const auto malicious_ostree_commit{addOstreeCommit()};
  TufTarget malicious_target{valid_target.Name(), malicious_ostree_commit, valid_target.Version(),
                             valid_target.Custom()};
  AkliteClient client(liteclient);

  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  auto latest = result.GetLatest();
  ASSERT_EQ(latest.Name(), malicious_target.Name());
  auto installer = client.Installer(malicious_target);
  ASSERT_EQ(nullptr, installer);
}

TEST_F(ApiClientTest, InstallTargetWithHackedApps) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  std::vector<AppEngine::App> apps{{"app-01", "app-01-URI"}};
  auto valid_target = Target::toTufTarget(createAppTarget(apps));
  auto malicious_apps{valid_target.AppsJson()};
  malicious_apps["app-01"]["uri"] = "malicious_app_uri";
  auto custom_data{valid_target.Custom()};
  custom_data[TufTarget::ComposeAppField] = malicious_apps;
  TufTarget malicious_target{valid_target.Name(), valid_target.Sha256Hash(), valid_target.Version(), custom_data};
  AkliteClient client(liteclient);

  auto result = client.CheckIn();
  ASSERT_EQ(CheckInResult::Status::Ok, result.status);
  auto latest = result.GetLatest();
  ASSERT_EQ(latest.Name(), malicious_target.Name());
  auto installer = client.Installer(malicious_target, "", "", InstallMode::OstreeOnly);
  ASSERT_EQ(nullptr, installer);
}

// Tests using Extended Aklite Client methods:
TEST_F(ApiClientTest, ExtApiRollback) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto new_target = createTarget();
  update(*liteclient, getInitialTarget(), new_target);

  AkliteClientExt client(liteclient);
  auto ci_res = client.CheckIn();
  auto result = client.GetTargetToInstall(ci_res);
  ASSERT_EQ(GetTargetToInstallResult::Status::Ok, result.status);
  ASSERT_FALSE(result.selected_target.IsUnknown());
  ASSERT_FALSE(client.IsRollback(result.selected_target));

  // deploy the initial version/commit to emulate rollback
  getSysRepo().deploy(getInitialTarget().sha256Hash());

  reboot(liteclient);
  // reboot re-creates an instance of LiteClient so `client` refers to an invalid/removed instance of LiteClient now,
  // hence we need to re-create an instance of AkliteClient
  AkliteClientExt rebooted_client(liteclient);

  ASSERT_TRUE(rebooted_client.IsRollback(result.selected_target));
  ASSERT_EQ(rebooted_client.GetCurrent().Sha256Hash(), getInitialTarget().sha256Hash());

  // Verify that GetTargetToInstall returns no target, because the latest one was already tried, and rolled back
  ci_res = rebooted_client.CheckIn();
  result = rebooted_client.GetTargetToInstall(ci_res);
  ASSERT_TRUE(result.selected_target.IsUnknown());
  ASSERT_EQ(result.status, GetTargetToInstallResult::Status::Ok);

  ASSERT_EQ(result.reason.find(new_target.filename() + " is a failing Target"), std::string::npos) << result.reason;
}

TEST_F(ApiClientTest, ExtApiInstallationInProgress) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto target1 = createTarget();
  update(*liteclient, getInitialTarget(), target1);
  reboot(liteclient);

  auto target2 = createTarget();
  AkliteClientExt client(liteclient);
  client.CompleteInstallation();
  auto ci_res = client.CheckIn();
  auto result = client.GetTargetToInstall(ci_res);
  ASSERT_EQ(GetTargetToInstallResult::Status::Ok, result.status);
  ASSERT_FALSE(result.selected_target.IsUnknown());
  ASSERT_EQ(target2.filename(), result.selected_target.Name());
  ASSERT_FALSE(client.IsRollback(result.selected_target));

  liteclient->config.pacman.booted = BootedType::kBooted;
  auto install_result = client.PullAndInstall(result.selected_target, result.reason);
  ASSERT_EQ(install_result.status, InstallResult::Status::NeedsCompletion);
  ASSERT_TRUE(client.RebootIfRequired());

  install_result = client.PullAndInstall(result.selected_target, result.reason);
  ASSERT_EQ(install_result.status, InstallResult::Status::InstallationInProgress);

  reboot(liteclient);
  // reboot re-creates an instance of LiteClient so `client` refers to an invalid/removed instance of LiteClient now,
  // hence we need to re-create an instance of AkliteClient
  AkliteClientExt rebooted_client(liteclient);
  install_result = rebooted_client.CompleteInstallation();
  ASSERT_EQ(install_result.status, InstallResult::Status::Ok);

  auto current = rebooted_client.GetCurrent();
  ASSERT_EQ(current.Sha256Hash(), target2.sha256Hash());
  ci_res = rebooted_client.CheckIn();
  result = rebooted_client.GetTargetToInstall(ci_res);
  ASSERT_TRUE(result.selected_target.IsUnknown());
  ASSERT_EQ(result.status, GetTargetToInstallResult::Status::Ok);
}

TEST_F(ApiClientTest, ExtApiSeparatePullAndInstall) {
  auto liteclient = createLiteClient();
  ASSERT_TRUE(targetsMatch(liteclient->getCurrent(), getInitialTarget()));

  // Create a new Target: update rootfs and commit it into Treehub's repo
  auto target1 = createTarget();
  update(*liteclient, getInitialTarget(), target1);
  reboot(liteclient);

  auto target2 = createTarget();
  AkliteClientExt client(liteclient);
  client.CompleteInstallation();
  auto ci_res = client.CheckIn();
  auto result = client.GetTargetToInstall(ci_res);
  ASSERT_EQ(GetTargetToInstallResult::Status::Ok, result.status);
  ASSERT_FALSE(result.selected_target.IsUnknown());
  ASSERT_EQ(target2.filename(), result.selected_target.Name());
  ASSERT_FALSE(client.IsRollback(result.selected_target));

  liteclient->config.pacman.booted = BootedType::kBooted;
  // Install without download, should fail
  auto install_result =
      client.PullAndInstall(result.selected_target, result.reason, "", InstallMode::All, nullptr, false, true);
  ASSERT_EQ(install_result.status, InstallResult::Status::DownloadFailed);

  // Download only
  install_result =
      client.PullAndInstall(result.selected_target, result.reason, "", InstallMode::All, nullptr, true, false);
  ASSERT_EQ(install_result.status, InstallResult::Status::Ok);

  // Install only
  install_result = client.PullAndInstall(result.selected_target, result.reason);
  ASSERT_EQ(install_result.status, InstallResult::Status::NeedsCompletion);

  reboot(liteclient);
  // reboot re-creates an instance of LiteClient so `client` refers to an invalid/removed instance of LiteClient now,
  // hence we need to re-create an instance of AkliteClient
  AkliteClientExt rebooted_client(liteclient);
  install_result = rebooted_client.CompleteInstallation();
  ASSERT_EQ(install_result.status, InstallResult::Status::Ok);

  auto current = rebooted_client.GetCurrent();
  ASSERT_EQ(current.Sha256Hash(), target2.sha256Hash());
  ci_res = rebooted_client.CheckIn();
  result = rebooted_client.GetTargetToInstall(ci_res);
  ASSERT_TRUE(result.selected_target.IsUnknown());
  ASSERT_EQ(result.status, GetTargetToInstallResult::Status::Ok);
}

int main(int argc, char** argv) {
  if (argc != 3) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  // options passed as args in CMakeLists.txt
  fixtures::DeviceGatewayMock::RunCmd = argv[1];
  fixtures::SysRootFS::CreateCmd = argv[2];
  return RUN_ALL_TESTS();
}
