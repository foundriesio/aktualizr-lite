#ifndef AKTUALIZR_LITE_CLIENT_H_
#define AKTUALIZR_LITE_CLIENT_H_

#include "gtest/gtest_prod.h"
#include "libaktualizr/config.h"
#include "libaktualizr/packagemanagerinterface.h"
#include "ostree/sysroot.h"
#include "uptane/fetcher.h"
#include "uptane/imagerepository.h"

class AppEngine;
class HttpClient;
class INvStorage;
class KeyManager;
class P11EngineGuard;
class ReportEvent;
class ReportQueue;
class DownloadResult;
class Downloader;

class LiteClient {
 public:
  explicit LiteClient(Config& config_in, const std::shared_ptr<AppEngine>& app_engine = nullptr,
                      const std::shared_ptr<P11EngineGuard>& p11 = nullptr,
                      std::shared_ptr<Uptane::IMetadataFetcher> meta_fetcher = nullptr);
  ~LiteClient();
  LiteClient(const LiteClient&) = delete;
  LiteClient& operator=(const LiteClient&) = delete;
  LiteClient(LiteClient&&) = default;
  LiteClient& operator=(LiteClient&&) = delete;

  Config config;
  std::vector<std::string> tags;
  std::shared_ptr<INvStorage> storage;

  std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> primary_ecu;
  std::shared_ptr<HttpClient> http_client;

  bool checkForUpdatesBegin();
  void checkForUpdatesEnd(const Uptane::Target& target);
  void checkForUpdatesEndWithFailure(const std::string& err);
  bool finalizeInstall();
  Uptane::Target getRollbackTarget();
  DownloadResult download(const Uptane::Target& target, const std::string& reason);
  data::ResultCode::Numeric install(const Uptane::Target& target);
  void notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir);
  std::pair<bool, std::string> isRebootRequired() const {
    return {is_reboot_required_, config.bootloader.reboot_command};
  }

  bool composeAppsChanged() const;
  Uptane::Target getCurrent() const { return package_manager_->getCurrent(); }
  std::tuple<bool, std::string> updateImageMeta();
  bool checkImageMetaOffline();
  const std::vector<Uptane::Target>& allTargets() const;
  TargetStatus VerifyTarget(const Uptane::Target& target) const { return package_manager_->verifyTarget(target); }
  void reportAktualizrConfiguration();
  void reportNetworkInfo();
  void reportHwInfo();
  void reportAppsState();
  bool isTargetActive(const Uptane::Target& target) const;
  bool appsInSync(const Uptane::Target& target) const;
  void setAppsNotChecked();
  std::string getDeviceID() const;
  static void update_request_headers(std::shared_ptr<HttpClient>& http_client, const Uptane::Target& target,
                                     PackageConfig& config);
  void logTarget(const std::string& prefix, const Uptane::Target& target) const;
  std::unique_ptr<ReportQueue> report_queue;
  bool isRollback(const Uptane::Target& target);

  void notifyDownloadFinished(const Uptane::Target& t, bool success, const std::string& err_msg = "");
  std::tuple<bool, boost::filesystem::path> isRootMetaImportNeeded();
  bool importRootMeta(const boost::filesystem::path& src, Uptane::Version max_ver = Uptane::Version());
  void importRootMetaIfNeededAndPresent();

 private:
  FRIEND_TEST(helpers, locking);
  FRIEND_TEST(helpers, callback);
  FRIEND_TEST(AkliteTest, RollbackIfAppsInstallFails);
  FRIEND_TEST(AkliteTest, RollbackIfAppsInstallFailsAndPowerCut);

  void callback(const char* msg, const Uptane::Target& install_target, const std::string& result = "");

  void notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event) const;
  void notifyDownloadStarted(const Uptane::Target& t, const std::string& reason);
  void notifyInstallStarted(const Uptane::Target& t);
  void writeCurrentTarget(const Uptane::Target& t) const;

  data::InstallationResult installPackage(const Uptane::Target& target);
  DownloadResult downloadImage(const Uptane::Target& target, const api::FlowControlToken* token = nullptr);
  static void add_apps_header(std::vector<std::string>& headers, PackageConfig& config);
  data::InstallationResult finalizePendingUpdate(boost::optional<Uptane::Target>& target);

  boost::filesystem::path callback_program;
  std::unique_ptr<KeyManager> key_manager_;
  std::shared_ptr<PackageManagerInterface> package_manager_;

  Uptane::ImageRepository image_repo_;
  std::shared_ptr<Uptane::IMetadataFetcher> uptane_fetcher_;

  Json::Value last_network_info_reported_;
  bool hwinfo_reported_{false};
  bool is_reboot_required_{false};

  std::shared_ptr<OSTree::Sysroot> sysroot_;
  std::vector<Uptane::Target> no_targets_;

  std::shared_ptr<Downloader> downloader_;
  Json::Value apps_state_;
  const int report_queue_run_pause_s_{10};
  const int report_queue_event_limit_{6};
};

#endif  // AKTUALIZR_LITE_CLIENT_H_
