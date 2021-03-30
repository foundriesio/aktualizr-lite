#ifndef AKTUALIZR_LITE_CLIENT_H_
#define AKTUALIZR_LITE_CLIENT_H_

#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "ostree/sysroot.h"
#include "package_manager/packagemanagerfake.h"
#include "primary/reportqueue.h"
#include "storage/invstorage.h"
#include "uptane/imagerepository.h"

class AppEngine;

class Lock {
 public:
  Lock(int fd) : fd_(fd) {}
  ~Lock() {
    if (fd_ != -1) {
      close(fd_);
    }
  }

 private:
  int fd_;
};

class LiteClient {
 public:
  LiteClient(Config& config_in, const std::shared_ptr<AppEngine>& app_engine = nullptr, bool finalize = true);

  Config config;
  std::vector<std::string> tags;
  std::shared_ptr<INvStorage> storage;

  std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> primary_ecu;
  std::shared_ptr<HttpClient> http_client;
  boost::filesystem::path download_lockfile;
  boost::filesystem::path update_lockfile;

  bool checkForUpdates();
  data::ResultCode::Numeric download(const Uptane::Target& target, const std::string& reason);
  data::ResultCode::Numeric install(const Uptane::Target& target);
  void notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir);
  std::pair<bool, std::string> isRebootRequired() const {
    return {is_reboot_required_, config.bootloader.reboot_command};
  }

  bool composeAppsChanged() const;
  Uptane::Target getCurrent() const { return package_manager_->getCurrent(); }
  bool updateImageMeta();
  bool checkImageMetaOffline();
  const std::vector<Uptane::Target>& allTargets() const { return image_repo_.getTargets()->targets; }
  TargetStatus VerifyTarget(const Uptane::Target& target) const { return package_manager_->verifyTarget(target); }
  void reportAktualizrConfiguration();
  void reportNetworkInfo();
  void reportHwInfo();
  bool isTargetActive(const Uptane::Target& target) const;
  bool appsInSync() const;
  void setAppsNotChecked();
  std::string getDeviceID() const;
  static void update_request_headers(std::shared_ptr<HttpClient>& http_client, const Uptane::Target& target,
                                     PackageConfig& config);
  bool finalizeInstallation();

  void logTarget(const std::string& prefix, const Uptane::Target& target) const;

 private:
  FRIEND_TEST(helpers, locking);
  FRIEND_TEST(helpers, callback);

  void callback(const char* msg, const Uptane::Target& install_target, const std::string& result = "");

  std::unique_ptr<Lock> getDownloadLock() const;
  std::unique_ptr<Lock> getUpdateLock() const;

  void notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event);
  void notifyDownloadStarted(const Uptane::Target& t, const std::string& reason);
  void notifyDownloadFinished(const Uptane::Target& t, bool success);
  void notifyInstallStarted(const Uptane::Target& t);

  void writeCurrentTarget(const Uptane::Target& t) const;

  data::InstallationResult installPackage(const Uptane::Target& target);
  std::pair<bool, Uptane::Target> downloadImage(const Uptane::Target& target,
                                                const api::FlowControlToken* token = nullptr);
  static void add_apps_header(std::vector<std::string>& headers, PackageConfig& config);

 private:
  boost::filesystem::path callback_program;
  std::unique_ptr<KeyManager> key_manager_;
  std::shared_ptr<PackageManagerInterface> package_manager_;
  std::unique_ptr<ReportQueue> report_queue;

  Uptane::ImageRepository image_repo_;
  std::shared_ptr<Uptane::Fetcher> uptane_fetcher_;

  Json::Value last_network_info_reported_;
  Json::Value last_hw_info_reported_;
  bool is_reboot_required_{false};
  bool booted_sysroot{true};
};

#endif  // AKTUALIZR_LITE_CLIENT_H_
