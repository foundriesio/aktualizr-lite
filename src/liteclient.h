#ifndef AKTUALIZR_LITE_CLIENT_H_
#define AKTUALIZR_LITE_CLIENT_H_

#include <boost/program_options.hpp>

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
  LiteClient(Config& config_in, const boost::program_options::variables_map* const variables_map = nullptr);

  void reportStatus();

  bool refreshMetadata();

  // Get currently installed (TODO: and running (active) Target)
  Uptane::Target getCurrent(bool refresh = false);

  /**
   * @brief Return a sorted map of applicable Targets
   *
   * Iterates through all available Targets and cherry picks those that matches
   * the given client hardware ID and tags.
   *
   * @return a sorted map of applicable Targets
   */
  boost::container::flat_map<int, Uptane::Target> getTargets();

  // Update a device to the given Target version
  data::ResultCode::Numeric update(const std::string& version = "latest", bool force_update = false);

  std::pair<bool, std::string> isRebootRequired() const;

  std::string getDeviceID() const;
  std::tuple<bool, Json::Value> getDeviceInfo();
  uint64_t updateInterval() const { return update_interval_; }

  void logTarget(const std::string& prefix, const Uptane::Target& t) const;

 private:
  FRIEND_TEST(helpers, locking);
  FRIEND_TEST(helpers, callback);
  FRIEND_TEST(helpers, rollback_versions);

  // reports device status
  void reportAktualizrConfiguration();
  void reportNetworkInfo();
  void reportHwInfo();

  // checking for TUF metadata, Target download and installation methods are subject for dedicated
  // class, e.g. TUFClient, it should be TUF client with no any use-case specifics
  // while LiteClient will encapsulate Foundries specifics of TUF application
  // checking/updating TUF metadata
  void checkForUpdates();
  bool updateImageMeta();
  bool checkImageMetaOffline();

  // getting TUF target(s) from a local storage
  std::unique_ptr<Uptane::Target> getTarget(const std::string& version = "latest");
  const std::vector<Uptane::Target>& allTargets() const { return image_repo_.getTargets()->targets; }
  bool isTargetValid(const Uptane::Target& target);

  // update helpers
  UpdateType determineUpdateType(const Uptane::Target& desired_target, const Uptane::Target& current_target,
                                 bool force_update);
  static Uptane::Target determineUpdateTarget(UpdateType update_type, const Uptane::Target& desired_target,
                                              const Uptane::Target& current_target);
  void logUpdate(UpdateType update_type, const Uptane::Target& desired_target,
                 const Uptane::Target& current_target) const;
  data::ResultCode::Numeric doUpdate(const Uptane::Target& desired_target, const Uptane::Target& udpate_target);

  // download Target
  data::ResultCode::Numeric download(const Uptane::Target& target, const std::string& reason);
  std::pair<bool, Uptane::Target> downloadImage(const Uptane::Target& target,
                                                const api::FlowControlToken* token = nullptr);
  std::unique_ptr<Lock> getDownloadLock() const;

  // install Target
  data::ResultCode::Numeric install(const Uptane::Target& target);
  data::InstallationResult installPackage(const Uptane::Target& target);
  void prune(const Uptane::Target& target);
  void writeCurrentTarget(const Uptane::Target& t) const;
  std::unique_ptr<Lock> getUpdateLock() const;

  // notify about update status via a callback (is subject for dedicated class)
  void notifyDownloadStarted(const Uptane::Target& t, const std::string& reason);
  void notifyDownloadFinished(const Uptane::Target& t, bool success);
  void notifyInstallStarted(const Uptane::Target& t);
  void notifyInstallFinished(const Uptane::Target& t, data::InstallationResult& ir);

  // notify mechanism
  void notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event);
  void callback(const char* msg, const Uptane::Target& install_target, const std::string& result = "");

  // helpers ???
  static void addAppsHeader(std::vector<std::string>& headers, PackageConfig& config_);
  static void updateRequestHeaders(std::shared_ptr<HttpClient>& http_client_, const Uptane::Target& target,
                                   PackageConfig& config_);
  void setInvalidTargets();

 private:
  Config config_;
  std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> primary_ecu_;

  std::shared_ptr<INvStorage> storage_;

  boost::filesystem::path download_lockfile_;
  boost::filesystem::path update_lockfile_;
  uint64_t update_interval_;
  std::vector<std::string> tags_;
  boost::filesystem::path callback_program_;

  std::shared_ptr<HttpClient> http_client_;
  std::shared_ptr<Uptane::Fetcher> uptane_fetcher_;
  std::unique_ptr<ReportQueue> report_queue_;
  std::unique_ptr<KeyManager> key_manager_;
  std::shared_ptr<PackageManagerInterface> package_manager_;

  std::vector<Uptane::Target> invalid_targets_;
  Json::Value last_network_info_reported_;
  Json::Value last_hw_info_reported_;
  Uptane::ImageRepository image_repo_;
  bool is_reboot_required_{false};
  bool booted_sysroot{true};
  Uptane::Target current_target_{Uptane::Target::Unknown()};
};

#endif  // AKTUALIZR_LITE_CLIENT_H_
