#include "primary/reportqueue.h"
#include "crypto/p11engine.h"
#include "http/httpclient.h"


namespace fixtures {

#include "liteclient/executecmd.cc"
#include "liteclient/sysrootfs.cc"
#include "liteclient/ostreerepomock.cc"
#include "liteclient/sysostreerepomock.cc"
#include "liteclient/tufrepomock.cc"
#include "liteclient/devicegatewaymock.cc"

class ClientTest :virtual public ::testing::Test {
 public:
  static std::string SysRootSrc;

 protected:
  ClientTest(std::string certs_dir = "")
      : sys_rootfs_{(test_dir_.Path() / "sysroot-fs").string(), branch, hw_id, os},
        sys_repo_{(test_dir_.Path() / "sysrepo").string(), os},
        tuf_repo_{test_dir_.Path() / "repo"},
        ostree_repo_{(test_dir_.Path() / "treehub").string(), true},
        device_gateway_{ostree_repo_, tuf_repo_, certs_dir},
        sysroot_hash_{sys_repo_.getRepo().commit(sys_rootfs_.path, sys_rootfs_.branch)},
        initial_target_{"unknown", Uptane::EcuMap(), {Hash(Hash::Type::kSha256, sysroot_hash_)}, 0, "", "OSTREE"} {
    sys_repo_.deploy(sysroot_hash_);
  }

  enum class InitialVersion { kOff, kOn, kCorrupted1, kCorrupted2 };
  enum class UpdateType { kOstree, kApp, kDownloadFailed, kOstreeApply, kFailed, kFinalized };


  virtual std::shared_ptr<LiteClient> createLiteClient(InitialVersion initial_version = InitialVersion::kOn,
                                                       boost::optional<std::vector<std::string>> apps = boost::none,
                                                       bool finalize = true) = 0;

  /**
   * method createLiteClient
   */
  std::shared_ptr<LiteClient> createLiteClient(const std::shared_ptr<AppEngine>& app_engine,
                                               InitialVersion initial_version = InitialVersion::kOn,
                                               boost::optional<std::vector<std::string>> apps = boost::none,
                                               const std::string& compose_apps_root = "",
                                               boost::optional<std::vector<std::string>> reset_apps = boost::none,
                                               bool create_containers_before_reboot = true, bool finalize = true) {
    Config conf;
    conf.tls.server = device_gateway_.getTlsUri();
    conf.uptane.repo_server = device_gateway_.getTufRepoUri();
    conf.provision.primary_ecu_hardware_id = hw_id;
    conf.storage.path = test_dir_.Path();
    conf.storage.uptane_metadata_path = utils::BasedPath(tuf_repo_.getRepoPath());

    conf.pacman.type = ComposeAppManager::Name;
    conf.pacman.sysroot = sys_repo_.getPath();
    conf.pacman.os = os;
    conf.pacman.booted = BootedType::kStaged;
    conf.pacman.extra["compose_apps_root"] = compose_apps_root.empty() ? (test_dir_.Path() / "compose-apps").string() : compose_apps_root;
    if (!!apps) {
      conf.pacman.extra["compose_apps"] = boost::algorithm::join(*apps, ",");
    }
    if (!!reset_apps) {
      conf.pacman.extra["reset_apps"] = boost::algorithm::join(*reset_apps, ",");
    }
    app_shortlist_ = apps;
    conf.pacman.ostree_server = device_gateway_.getOsTreeUri();
    if (!create_containers_before_reboot) {
      // by default it set to "1"/true in the composeappmanager's config
      conf.pacman.extra["create_containers_before_reboot"] = "0";
    }

    conf.bootloader.reboot_command = "/bin/true";
    conf.bootloader.reboot_sentinel_dir = conf.storage.path;
    conf.import.base_path = test_dir_ / "import";

    if (initial_version == InitialVersion::kOn || initial_version == InitialVersion::kCorrupted1 ||
        initial_version == InitialVersion::kCorrupted2) {
      /*
       * Sample LMP/OE generated installed_version file
       *
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

      Json::Value installed_version;
      // corrupted1 will invalidate the sysroot_hash_ sha256
      installed_version["hashes"]["sha256"] =
          sysroot_hash_ + (initial_version == InitialVersion::kCorrupted1 ? "DEADBEEF" : "");
      installed_version["is_current"] = true;
      installed_version["custom"]["name"] = hw_id + "-" + os;
      installed_version["custom"]["version"] = "1";
      installed_version["custom"]["hardwareIds"] = hw_id;
      installed_version["custom"]["targetFormat"] = "OSTREE";
      installed_version["custom"]["arch"] = "aarch64";
      installed_version["custom"]["image-file"] = "lmp-factory-image-raspberrypi4-64.wic.gz";
      installed_version["custom"]["tags"] = "master";

      /* create the initial_target from the above json file: pass the root node
       * name as a parameter
       */
      initial_target_ = Uptane::Target{hw_id + "-" + os + "-" + "1", installed_version};
      // emulate Foundries real Target, add custom.uri attribute
      Json::Value custom_value;
      custom_value["uri"] = "https://ci.foundries.io/projects/factory/lmp/builds/1097";
      initial_target_.updateCustom(custom_value);

      Json::Value ins_ver;
      // set the root node name
      ins_ver[initial_target_.filename()] = installed_version;
      // write the json information to a file (corrupted2 will write a corrupted  file)
      Utils::writeFile(conf.import.base_path / "installed_versions",
                       (initial_version == InitialVersion::kCorrupted2) ? "deadbeef\t\ncorrupted file\n\n"
                                                                        : Utils::jsonToCanonicalStr(ins_ver),
                       true);

      getTufRepo().addTarget(initial_target_.filename(), initial_target_.sha256Hash(), hw_id, "1");
    }

    auto client = std::make_shared<LiteClient>(conf, app_engine);

    // import root metadata
    const auto import{client->isRootMetaImportNeeded()};
    if (std::get<0>(import)) {
      // rotate root twice to emulate real-life use-case (initial rotate and user's first rotation)
      tuf_repo_.repo().rotate(Uptane::Role::Root());
      tuf_repo_.repo().rotate(Uptane::Role::Root());

      LOG_INFO << "Importing root role metadata...";
      if (!client->importRootMeta(std::get<1>(import))) {
        throw std::runtime_error("Failed to import root metadata");
      }
      // another rotation in case if root was rotated before a device initial boot
      tuf_repo_.repo().rotate(Uptane::Role::Root());
    }

    if (finalize) {
      client->finalizeInstall();
    }
    return client;
  }

  /**
   * method createTarget
   */
  Uptane::Target createTarget(const std::vector<AppEngine::App>* apps = nullptr, std::string hwid = "",
                              const std::string& rootfs_path = "", boost::optional<TufRepoMock&> tuf_repo = boost::none,
                              const std::string& ver = "") {
    auto& repo{!!tuf_repo?*tuf_repo:getTufRepo()};
    const auto& latest_target{repo.getLatest()};
    std::string version{ver};
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
    std::string rootfs{rootfs_path};
    if (rootfs.empty()) {
      rootfs = getSysRootFs().path;
    }
    Utils::writeFile(rootfs + "/" + unique_file, unique_content, true);
    auto hash = getOsTreeRepo().commit(rootfs, "lmp");

    Json::Value apps_json;
    if (apps) {
      for (const auto& app : *apps) {
        apps_json[app.name]["uri"] = app.uri;
      }
    }

    if (hwid.empty()) {
      hwid = hw_id;
    }

    // add new target to TUF repo
    const std::string name = hwid + "-" + os + "-" + version;
    return repo.addTarget(name, hash, hwid, version, apps_json);
  }

  /**
   * method createAppTarget
   */
  Uptane::Target createAppTarget(const std::vector<AppEngine::App>& apps, const Uptane::Target& base_target = Uptane::Target::Unknown()) {
    Uptane::Target base{base_target};
    if (targetsMatch(base, Uptane::Target::Unknown())) {
      base = getTufRepo().getLatest();
    }

    const std::string version = std::to_string(std::stoi(base.custom_version()) + 1);
    Json::Value apps_json;
    for (const auto& app : apps) {
      apps_json[app.name]["uri"] = app.uri;
    }

    // add new target to TUF repo
    const std::string name = hw_id + "-" + os + "-" + version;
    return getTufRepo().addTarget(name, base.sha256Hash(), hw_id, version, apps_json);
  }

  /**
   * method createApp
   */
  AppEngine::App createApp(
      const std::string& name, const std::string& factory = "test-factory",
      const std::string& hash = "7ca42b1567ca068dfd6a5392432a5a36700a4aa3e321922e91d974f832a2f243") {
    const std::string uri =
        "localhost:" + getDeviceGateway().getPort() + "/" + factory + "/" + name + "@sha256:" + hash;
    return {name, uri};
  }

  /**
   * mehod update
   */
  void update(LiteClient& client, const Uptane::Target& from, const Uptane::Target& to,
              data::ResultCode::Numeric expected_install_code = data::ResultCode::Numeric::kNeedCompletion,
              DownloadResult expected_download_result = {DownloadResult::Status::Ok, ""}, const std::string& expected_install_err_msg = "") {
    device_gateway_.resetEvents();
    // TODO: remove it once aklite is moved to the newer version of LiteClient that exposes update() method
    ASSERT_TRUE(client.checkForUpdatesBegin());

    // TODO: call client->getTarget() once the method is moved to LiteClient
    const auto download_result{client.download(to, "")};
    ASSERT_EQ(download_result.status, expected_download_result.status);
    ASSERT_TRUE(download_result.description.find(expected_download_result.description) != std::string::npos) << "Actuall error message: " << download_result.description;
    if (expected_download_result.noSpace()) {
      ASSERT_EQ(download_result.destination_path, sys_repo_.getPath());
    }
    if (download_result) {
      ASSERT_EQ(client.install(to), expected_install_code);
      // make sure that the new Target hasn't been applied/finalized before reboot
      ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from.filename());

      checkEvents(client, from, expected_install_code == data::ResultCode::Numeric::kNeedCompletion?UpdateType::kOstreeApply:UpdateType::kFailed, expected_download_result.description, expected_install_err_msg);
    } else {
      checkEvents(client, from, UpdateType::kDownloadFailed, expected_download_result.description);
    }
    checkHeaders(client, from);
  }

  /**
   * method updateApps
   */
  void updateApps(LiteClient& client, const Uptane::Target& from, const Uptane::Target& to,
                  DownloadResult::Status expected_download_code = DownloadResult::Status::Ok,
                  const std::string& download_err_msg = "",
                  data::ResultCode::Numeric expected_install_code = data::ResultCode::Numeric::kOk,
                  const std::string& install_err_msg = "") {
    device_gateway_.resetEvents();
    // TODO: remove it once aklite is moved to the newer version of LiteClient that exposes update() method
    ASSERT_TRUE(client.checkForUpdatesBegin());

    // TODO: call client->getTarget() once the method is moved to LiteClient
    const DownloadResult dr{client.download(to, "")};
    ASSERT_EQ(dr.status, expected_download_code);
    ASSERT_NE(dr.description.find(download_err_msg), std::string::npos) << "Actuall error message: " << dr.description;

    if (expected_download_code != DownloadResult::Status::Ok) {
      ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from.filename());
      checkHeaders(client, from);
      checkEvents(client, from, UpdateType::kDownloadFailed, download_err_msg);
      return;
    }

    if (client.VerifyTarget(to) != TargetStatus::kGood) {
      ASSERT_EQ(expected_install_code, data::ResultCode::Numeric::kVerificationFailed);
      ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from.filename());
      checkHeaders(client, from);
      checkEvents(client, from, UpdateType::kDownloadFailed);
      return;
    }

    ASSERT_EQ(client.install(to), expected_install_code);
    if (expected_install_code == data::ResultCode::Numeric::kOk) {
      // make sure that the new Target has been applied
      ASSERT_EQ(client.getCurrent().sha256Hash(), to.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), to.filename());
      // TODO: the daemon_main is emulated,
      // see
      // https://github.com/foundriesio/aktualizr-lite/blob/7ab6998920d57605601eda16f9bebedf00cc1f7f/src/main.cc#L264
      // once the daemon_main is "cleaned" the updateHeader can be removed from the test.
      LiteClient::update_request_headers(client.http_client, to, client.config.pacman);
      checkHeaders(client, to);
      checkEvents(client, to, UpdateType::kApp);
    } else {
      ASSERT_EQ(client.getCurrent().sha256Hash(), from.sha256Hash());
      ASSERT_EQ(client.getCurrent().filename(), from.filename());
      checkHeaders(client, from);
      checkEvents(client, from, UpdateType::kApp, "", install_err_msg);
    }
  }

  /**
   * method targetsMatch
   */
  bool targetsMatch(const Uptane::Target& lhs, const Uptane::Target& rhs) {


    if ((lhs.sha256Hash() != rhs.sha256Hash()) || (lhs.filename() != rhs.filename())) {
      return false;
    }

    auto lhs_custom = Target::appsJson(lhs);
    auto rhs_custom = Target::appsJson(rhs);

    if (lhs_custom == Json::nullValue && rhs_custom == Json::nullValue) {
      return true;
    }

    if ((lhs_custom != Json::nullValue && rhs_custom == Json::nullValue) ||
        (lhs_custom == Json::nullValue && rhs_custom != Json::nullValue)) {
      return false;
    }

    for (Json::ValueConstIterator app_it = lhs_custom.begin(); app_it != lhs_custom.end(); ++app_it) {
      if (!(*app_it).isObject() || !(*app_it).isMember("uri")) {
        continue;
      }

      const auto& app_name = app_it.key().asString();
      const auto& app_uri = (*app_it)["uri"].asString();
      if (!rhs_custom.isMember(app_name) || rhs_custom[app_name]["uri"] != app_uri) {
        return false;
      }
    }
    return true;
  }

  /**
   * method reboot
   */
  void reboot(std::shared_ptr<LiteClient>& client, boost::optional<std::vector<std::string>> new_app_list = boost::none) {
    boost::filesystem::remove(test_dir_.Path() / "need_reboot");
    // make sure we tear down an existing instance of a client before a new one is created
    // otherwise we hit race condition with sending events by the report queue thread, the same event is sent twice
    client.reset();
    if (!!new_app_list) {
      app_shortlist_ = new_app_list;
    }
    client = createLiteClient(InitialVersion::kOff, app_shortlist_);
  }

  /**
   * method restart
   */
  void restart(std::shared_ptr<LiteClient>& client) { client = createLiteClient(InitialVersion::kOff); }

  /**
   * method checkHeaders
   */
  void checkHeaders(LiteClient& client, const Uptane::Target& target) {
    // check for a new Target in order to send requests with headers we are interested in
    ASSERT_TRUE(client.checkForUpdatesBegin());
    if (target.MatchTarget(Uptane::Target::Unknown())) return;

    auto req_headers = getDeviceGateway().getReqHeaders();
    ASSERT_EQ(req_headers["x-ats-ostreehash"], target.sha256Hash());
    ASSERT_EQ(req_headers["x-ats-target"], target.filename());
    ASSERT_EQ(req_headers.get("x-ats-dockerapps", ""), Target::appsStr(target, app_shortlist_));
  }

  void checkEvents(LiteClient& client, const Uptane::Target& target, UpdateType update_type, const std::string& download_failure_err_msg = "", const std::string& install_failure_err_msg = "") {
    // Before checking events we need to make sure that the ReportQueue actually has drained all events.
    // Unfortunatelly, it doesn't expose any public method to do it, so we need either `sleep()` here
    // or re-create the report queueu instance. Sleeping tests running time longer and it's not reliable
    // since we don't know how much time we really need to sleep in order to drain all events.
    // Therefore, let's re-create the queue instance. Unfortunatelly, it uncovers the sync issue in ReportQueue.
    // Specifically, it may cause sending of the same event(s) twice because the `ReportQueue::flushQueue()`
    // invocations in the dtor and the ReportQueue::run() are not synced or/and deleting events from the DB.
    // So, if we do the event draining by the queue instance re-creation then we need to fix/improve
    // the Device Gateway mock so it removes even duplicates.
    // sleep(2);
    client.report_queue = std::make_unique<ReportQueue>(client.config, client.http_client, client.storage, 0, 1);

    const std::unordered_map<UpdateType, std::vector<std::string>> updateToevents = {
        { UpdateType::kOstree, { "EcuDownloadStarted", "EcuDownloadCompleted", "EcuInstallationStarted", "EcuInstallationApplied", "EcuInstallationCompleted" }},
        { UpdateType::kApp, { "EcuDownloadStarted", "EcuDownloadCompleted", "EcuInstallationStarted", "EcuInstallationCompleted" }},
        { UpdateType::kDownloadFailed, { "EcuDownloadStarted", "EcuDownloadCompleted" }},
        { UpdateType::kOstreeApply, { "EcuDownloadStarted", "EcuDownloadCompleted", "EcuInstallationStarted", "EcuInstallationApplied" }},
        { UpdateType::kFailed, { "EcuDownloadStarted", "EcuDownloadCompleted", "EcuInstallationStarted", "EcuInstallationCompleted" }},
        { UpdateType::kFinalized, { "EcuInstallationCompleted" }},
    };
    const std::vector<std::string>& expected_events{updateToevents.at(update_type)};
    auto expected_event_it = expected_events.begin();
    // wait a bit to make sure all events arrive at Device Gateway before making the following call
    std::this_thread::sleep_for(std::chrono::milliseconds(20));
    auto events = getDeviceGateway().getEvents();
    ASSERT_EQ(expected_events.size(), events.size());
    for (auto rec_event_it = events.begin(); rec_event_it != events.end(); ++rec_event_it) {
      const auto& rec_event_json = *rec_event_it;
      const auto event_type = rec_event_json["eventType"]["id"].asString();
      ASSERT_TRUE(expected_event_it != expected_events.end());
      ASSERT_EQ(*expected_event_it, event_type);
      ++expected_event_it;
      if (event_type == "EcuInstallationCompleted") {
        // TODO: check whether a value of ["event"]["details"] macthes the current Target
        // makes sense to represent it as a json string
        const auto event_details = rec_event_json["event"]["details"].asString();
        ASSERT_TRUE(event_details.find("Apps running:") != std::string::npos);
        ASSERT_TRUE(event_details.find(install_failure_err_msg) != std::string::npos) << event_details;
      }
      if (event_type == "EcuDownloadCompleted") {
        const auto event_details = rec_event_json["event"]["details"].asString();
        ASSERT_TRUE(event_details.find(download_failure_err_msg) != std::string::npos) << event_details;
      }
    }
  }

  /**
   * methods: miscellaneous
   */
  void setInitialTarget(const Uptane::Target& target) { initial_target_ = target; }
  const Uptane::Target& getInitialTarget() const { return initial_target_; }

  DeviceGatewayMock& getDeviceGateway() { return device_gateway_; }
  SysOSTreeRepoMock& getSysRepo() { return sys_repo_; }
  SysRootFS& getSysRootFs() { return sys_rootfs_; }
  TufRepoMock& getTufRepo() { return tuf_repo_; }
  OSTreeRepoMock& getOsTreeRepo() { return ostree_repo_; }
  void setAppShortlist(const std::vector<std::string>& apps) { app_shortlist_ = boost::make_optional(apps); }

 protected:
  static const std::string branch;
  static const std::string hw_id;
  static const std::string os;

 protected:
  TemporaryDirectory test_dir_;  // must be the first element in the class
  SysRootFS sys_rootfs_;
  SysOSTreeRepoMock sys_repo_;
  TufRepoMock tuf_repo_;
  OSTreeRepoMock ostree_repo_;
  DeviceGatewayMock device_gateway_;
  const std::string sysroot_hash_;
  Uptane::Target initial_target_;

  boost::optional<std::vector<std::string>> app_shortlist_;
};

std::string ClientTest::SysRootSrc;
const std::string ClientTest::branch{"lmp"};
const std::string ClientTest::hw_id{"raspberrypi4-64"};
const std::string ClientTest::os{"lmp"};

} // namespace fixtures
