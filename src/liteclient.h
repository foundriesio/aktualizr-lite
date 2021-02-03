#ifndef AKTUALIZR_LITE_CLIENT_H_
#define AKTUALIZR_LITE_CLIENT_H_

#include "http/httpclient.h"
#include "libaktualizr/config.h"
#include "ostree/sysroot.h"
#include "package_manager/packagemanagerfake.h"
#include "primary/reportqueue.h"
#include "storage/invstorage.h"
#include "uptane/imagerepository.h"

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
  enum UpdateType { kNewTargetUpdate = 0, kCurrentTargetSync, kTargetForcedUpdate };

 public:
  LiteClient(Config& config_in);

  // TODO: consider making the following members private
  Config config;
  std::shared_ptr<HttpClient> http_client;
  boost::filesystem::path download_lockfile;
  boost::filesystem::path update_lockfile;

 public:
  // Get currently installed and running (active) Target
  Uptane::Target getCurrent(bool force_check = false);

  // Check for new TUF metadata at the TUF server
  bool checkForUpdates();

  // Get Target of a specific version
  std::unique_ptr<Uptane::Target> getTarget(const std::string& version = "latest");

  // Get all available Targets
  boost::container::flat_map<int, Uptane::Target> getTargets();

  // Check if it's a new Target
  bool isNewAndAllowedTarget(const Uptane::Target& target);

  // Update a device with the given Target
  data::ResultCode::Numeric update(Uptane::Target& target, bool forced_update = false);

  // Check if a syste reboot is required after an update
  std::pair<bool, std::string> isRebootRequired() const {
    return {is_reboot_required_, config.bootloader.reboot_command};
  }

  // TODO
  bool updateImageMeta();
  // TODO
  bool checkImageMetaOffline();

  // Device status reporting, make it private once the update loop is moved under LiteClient
  void reportAktualizrConfiguration();
  void reportNetworkInfo();
  void reportHwInfo();

  std::string getDeviceID() const;
  void logTarget(const std::string& prefix, const Uptane::Target& t) const;

 private:
  FRIEND_TEST(helpers, locking);
  FRIEND_TEST(helpers, callback);

  const std::vector<Uptane::Target>& allTargets() const { return image_repo_.getTargets()->targets; }

  std::unique_ptr<Lock> getDownloadLock() const;
  data::ResultCode::Numeric download(const Uptane::Target& target, const std::string& reason);
  std::pair<bool, Uptane::Target> downloadImage(const Uptane::Target& target,
                                                const api::FlowControlToken* token = nullptr);

  std::unique_ptr<Lock> getUpdateLock() const;
  data::ResultCode::Numeric install(Uptane::Target& target_to_install, Uptane::Target& target);
  data::InstallationResult installPackage(const Uptane::Target& target);
  void writeCurrentTarget(const Uptane::Target& t) const;

  void prune(Uptane::Target& target);

  void callback(const char* msg, const Uptane::Target& install_target, const std::string& result = "");

  void notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event);
  void notifyDownloadStarted(const Uptane::Target& t, const std::string& reason);
  void notifyDownloadFinished(const Uptane::Target& t, bool success);
  void notifyInstallStarted(const Uptane::Target& t);
  void notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir);

  static void add_apps_header(std::vector<std::string>& headers, PackageConfig& config);

  static void update_request_headers(std::shared_ptr<HttpClient>& http_client, const Uptane::Target& target,
                                     PackageConfig& config);

  static void generateCorrelationId(Uptane::Target& t);

  // TODO: why it's needed here at all, should me moved under package manager
  TargetStatus VerifyTarget(const Uptane::Target& target) const { return package_manager_->verifyTarget(target); }

 private:
  std::shared_ptr<INvStorage> storage_;
  std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> primary_ecu_;
  std::vector<std::string> tags_;

  boost::filesystem::path callback_program;
  std::unique_ptr<KeyManager> key_manager_;
  std::shared_ptr<PackageManagerInterface> package_manager_;
  std::unique_ptr<ReportQueue> report_queue_;

  Uptane::ImageRepository image_repo_;
  std::shared_ptr<Uptane::Fetcher> uptane_fetcher_;

  Json::Value last_network_info_reported_;
  Json::Value last_hw_info_reported_;
  bool is_reboot_required_{false};
  bool booted_sysroot{true};
  std::vector<Uptane::Target> installed_versions_;

  Uptane::Target current_target_{Uptane::Target::Unknown()};
};

#endif  // AKTUALIZR_LITE_CLIENT_H_
