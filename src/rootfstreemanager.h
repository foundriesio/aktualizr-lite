#ifndef AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
#define AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_

#include "aktualizr-lite/storage/stat.h"
#include "bootloader/bootloaderlite.h"
#include "downloader.h"
#include "http/httpinterface.h"
#include "installer.h"
#include "ostree/sysroot.h"
#include "package_manager/ostreemanager.h"

class RootfsTreeManager : public OstreeManager, public Downloader, public Installer {
 public:
  static constexpr const char* const Name{"ostree"};
  struct Config {
   public:
    explicit Config(const PackageConfig& pconfig);

    static constexpr const char* const UpdateBlockParamName{"ostree_update_block"};
    static constexpr const char* const OstreePullToolParamName{"ostree_pull_tool"};

    // A flag enabling/disabling ostree update blocking if there is ongoing boot firmware update
    // that requires confirmation by means of reboot.
    bool UpdateBlock{true};

    // Selects the ostree pull implementation and the helper used to estimate an
    // update's required storage. Empty (default) or "libostree" uses the built-in
    // libostree pull and no external size helper. Any other value is the `fiopull`
    // helper to exec — either a bare binary name looked up on $PATH (e.g.
    // "fiopull") or an absolute path (e.g. "/usr/bin/fiopull"). The fiopull path
    // automatically falls back to libostree when the binary cannot be found or the
    // remote needs credentials fiopull cannot present (e.g. a PKCS#11-backed key).
    std::string OstreePullTool;
  };
  using RequestHeaders = std::unordered_map<std::string, std::string>;
  struct Remote {
    std::string name;
    std::string baseUrl;
    RequestHeaders headers;
    boost::optional<const KeyManager*> keys;
    bool isRemoteSet{false};
  };

  RootfsTreeManager(const PackageConfig& pconfig, const BootloaderConfig& bconfig,
                    const std::shared_ptr<INvStorage>& storage, const std::shared_ptr<HttpInterface>& http,
                    std::shared_ptr<OSTree::Sysroot> sysroot, const KeyManager& keys);

  DownloadResult Download(const TufTarget& target) override;
  data::InstallationResult Install(const TufTarget& target, InstallMode mode) override;

  bool fetchTarget(const Uptane::Target& target, Uptane::Fetcher& fetcher, const KeyManager& keys,
                   const FetcherProgressCb& progress_cb, const api::FlowControlToken* token) override;

  const bootloader::BootFwUpdateStatus& bootFwUpdateStatus() const { return *boot_fw_update_status_; }
  void setInitialTargetIfNeeded(const std::string& hw_id);
  data::InstallationResult install(const Uptane::Target& target) const override;

 protected:
  virtual void completeInitialTarget(Uptane::Target& init_target) {};
  void installNotify(const Uptane::Target& target) override;
  const std::shared_ptr<OSTree::Sysroot>& sysroot() const { return sysroot_; }

  // OstreeUpdateSize reports the storage the ostree commit update needs.
  // `known` is false when no size could be estimated (no delta-stats published
  // and no static delta available); the caller then proceeds without an ostree
  // contribution to the combined space check. `required` is the uncompressed
  // (on-disk) size in bytes; `path` is the ostree repo path (its volume).
  struct OstreeUpdateSize {
    bool known{false};
    uint64_t required{0};
    std::string path;
  };
  // Estimates the ostree commit update's required storage without pulling, by
  // querying the same delta-size sources Download() uses (the published
  // delta-stats file, falling back to the fiopull superblock reader) over the
  // target's remotes. Returns known=false if none yields a size.
  OstreeUpdateSize getOstreeUpdateSize(const TufTarget& target);

 private:
  struct DeltaStatsRef {
    std::string sha256;
    unsigned int size;
  };
  struct DeltaStat {
    uint64_t size;
    uint64_t uncompressedSize;
  };
  struct StorageStat {
    uint64_t blockSize;
    uint64_t freeBlockNumb;
    uint64_t blockNumb;
  };

  std::string getCurrentHash() const override {
    return sysroot_->getDeploymentHash(OSTree::Sysroot::Deployment::kCurrent);
  }
  void getAdditionalRemotes(std::vector<Remote>& remotes, const std::string& target_name);
  // Builds the ordered list of remotes to fetch the ostree commit from: the
  // base gateway remote, plus any additional (e.g. GCS) remotes obtained from
  // the gateway when the ostree server is an http(s) endpoint.
  std::vector<Remote> getRemotes(const std::string& target_name);

  void setRemote(const std::string& name, const std::string& url, const boost::optional<const KeyManager*>& keys);
  data::InstallationResult verifyBootloaderUpdate(const Uptane::Target& target) const;
  bool getDeltaStatIfAvailable(const TufTarget& target, const Remote& remote, DeltaStat& delta_stat) const;
  // Estimates the update's required size by invoking the `fiopull` helper's
  // `update-size`, which picks the cheapest accurate source itself (static delta
  // -> commit ostree.sizes metadata), so a size is produced even for factories
  // that publish no delta-stats file. Online remotes are queried over http(s);
  // for an offline update the local sysroot repo is read via a file:// URL.
  // Returns false if the helper is unavailable, the remote is neither http(s)
  // nor file://, or no size could be determined.
  bool getDeltaStatFromFioPull(const TufTarget& target, const Remote& remote, DeltaStat& delta_stat) const;
  // Reports whether the fiopull pull path should be used for this remote: the
  // ostree_pull_tool config names a helper that resolves to an existing binary
  // (fioPullBin() non-empty) and the remote needs no credentials fiopull cannot
  // present (fiopull cannot use a PKCS#11-backed key/cert/CA). Falls back to
  // libostree otherwise.
  bool useFioPull(const Remote& remote) const;
  // Resolves the configured ostree_pull_tool to an absolute path to the fiopull
  // binary, or "" when fiopull is disabled (empty/"libostree") or the binary
  // cannot be found. A bare name is looked up on $PATH; an absolute path is used
  // as-is if it exists. The result is cached on first use.
  std::string fioPullBin() const;
  // Pulls the target's ostree commit by exec'ing `<fiopull> pull` into the
  // sysroot repo, passing the remote's headers and (for an mTLS remote) the
  // KeyManager's CA/cert/key file paths, plus the current commit as --from so the
  // helper can use a static delta. Returns a data::InstallationResult mirroring
  // OstreeManager::pull (kOk / kInstallFailed), with the no-space exit code
  // surfaced so the caller can map it to DownloadFailed_NoSpace.
  data::InstallationResult pullWithFioPull(const TufTarget& target, const Remote& remote) const;
  storage::Volume::UsageInfo getUsageInfo() const;

  static bool getDeltaStatsRef(const Json::Value& json, DeltaStatsRef& ref);
  static Json::Value downloadDeltaStats(const DeltaStatsRef& ref, const Remote& remote);
  static bool findDeltaStatForUpdate(const Json::Value& delta_stats, const std::string& from, const std::string& to,
                                     DeltaStat& found_delta_stat);

  const KeyManager& keys_;
  std::shared_ptr<OSTree::Sysroot> sysroot_;
  std::unique_ptr<bootloader::BootFwUpdateStatus> boot_fw_update_status_;
  std::shared_ptr<HttpInterface> http_client_;
  const std::string gateway_url_;
  const Config cfg_;
  // Cached result of fioPullBin(): the resolved absolute path to the fiopull
  // binary, or "" when disabled/not found. Resolved lazily on first use.
  mutable boost::optional<std::string> fio_pull_bin_;
};

#endif  // AKTUALIZR_LITE_ROOTFS_TREE_MANAGER_H_
