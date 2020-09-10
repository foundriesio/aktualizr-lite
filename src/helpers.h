#ifndef AKTUALIZR_LITE_HELPERS_
#define AKTUALIZR_LITE_HELPERS_

#include <string>

#include <string.h>

#include "primary/sotauptaneclient.h"
#include "uptane/tuf.h"

struct Version {
  std::string raw_ver;
  Version(std::string version) : raw_ver(std::move(version)) {}

  bool operator<(const Version& other) { return strverscmp(raw_ver.c_str(), other.raw_ver.c_str()) < 0; }
};

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

// TODO:
// 1) move LiteClient implementation out of helpers.* to lite_client.*
// 2) improve implementation: makes fields private, get rid of aktualizr's redundant legacy,
//  restructure code in order to minimize code duplication and improve on loose coupling, etc

class LiteClient {
 public:
  LiteClient(Config& config_in);

  Config config;
  std::vector<std::string> tags;
  std::shared_ptr<INvStorage> storage;

  std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> primary_ecu;
  std::shared_ptr<HttpClient> http_client;
  boost::filesystem::path download_lockfile;
  boost::filesystem::path update_lockfile;

  bool checkForUpdates();
  data::ResultCode::Numeric download(const Uptane::Target& target);
  data::ResultCode::Numeric install(const Uptane::Target& target);
  void notifyInstallFinished(const Uptane::Target& t, data::ResultCode::Numeric rc);
  std::pair<bool, std::string> isRebootRequired() const {
    return {is_reboot_required_, config.bootloader.reboot_command};
  }

  bool dockerAppsChanged(bool check_target_apps = true);
  void storeDockerParamsDigest();
  Uptane::Target getCurrent() const { return package_manager_->getCurrent(); }
  bool updateImageMeta();
  bool checkImageMetaOffline();
  Uptane::LazyTargetsList allTargets() const { return Uptane::LazyTargetsList(image_repo_, storage, uptane_fetcher_); }
  TargetStatus VerifyTarget(const Uptane::Target& target) const { return package_manager_->verifyTarget(target); }
  void reportAktualizrConfiguration();
  void reportNetworkInfo();
  void reportHwInfo();
  bool isTargetCurrent(const Uptane::Target& target) const;
  bool checkAppsToUpdate(const Uptane::Target& target) const;
  void setAppsNotChecked();

 private:
  FRIEND_TEST(helpers, locking);
  FRIEND_TEST(helpers, callback);

  void callback(const char* msg, const Uptane::Target& install_target, const std::string& result = "");

  std::unique_ptr<Lock> getDownloadLock();
  std::unique_ptr<Lock> getUpdateLock();

  void notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event);
  void notifyDownloadStarted(const Uptane::Target& t);
  void notifyDownloadFinished(const Uptane::Target& t, bool success);
  void notifyInstallStarted(const Uptane::Target& t);

  void writeCurrentTarget(const Uptane::Target& t);

  data::InstallationResult installPackage(const Uptane::Target& target);
  std::pair<bool, Uptane::Target> downloadImage(const Uptane::Target& target,
                                                const api::FlowControlToken* token = nullptr);

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

void generate_correlation_id(Uptane::Target& t);
bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags);
bool targets_eq(const Uptane::Target& t1, const Uptane::Target& t2, bool compareDockerApps);
bool known_local_target(LiteClient& client, const Uptane::Target& t, std::vector<Uptane::Target>& installed_versions);
void log_info_target(const std::string& prefix, const Config& config, const Uptane::Target& t);
bool match_target_base(const Uptane::Target& t1, const Uptane::Target& t2);

#endif  // AKTUALIZR_LITE_HELPERS_
