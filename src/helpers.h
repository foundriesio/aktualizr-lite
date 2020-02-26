#ifndef AKTUALIZR_LITE_HELPERS
#define AKTUALIZR_LITE_HELPERS

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

  void release() {
    if (fd_ != -1) {
      close(fd_);
    }
  }

 private:
  int fd_;
};

struct LiteClient {
  LiteClient(Config& config_in);

  Config config;
  std::vector<std::string> tags;
  std::shared_ptr<INvStorage> storage;
  std::shared_ptr<SotaUptaneClient> primary;
  std::shared_ptr<PackageManagerInterface> package_manager;
  std::pair<Uptane::EcuSerial, Uptane::HardwareIdentifier> primary_ecu;
  std::unique_ptr<ReportQueue> report_queue;
  std::shared_ptr<HttpClient> http_client;
  boost::filesystem::path download_lockfile;
  boost::filesystem::path update_lockfile;

  std::unique_ptr<Lock> getDownloadLock();
  std::unique_ptr<Lock> getUpdateLock();

  void notifyDownloadStarted(const Uptane::Target& t);
  void notifyDownloadFinished(const Uptane::Target& t, bool success);
  void notifyInstallStarted(const Uptane::Target& t);
  void notifyInstallFinished(const Uptane::Target& t, data::ResultCode::Numeric rc);

  void notify(const Uptane::Target& t, std::unique_ptr<ReportEvent> event);
  bool dockerAppsChanged();
  void storeDockerParamsDigest();
};

bool should_compare_docker_apps(const Config& config);
void generate_correlation_id(Uptane::Target& t);
bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags);
bool targets_eq(const Uptane::Target& t1, const Uptane::Target& t2, bool compareDockerApps);
bool known_local_target(LiteClient& client, const Uptane::Target& t, std::vector<Uptane::Target>& installed_versions);

#endif  // AKTUALIZR_LITE_HELPERS
