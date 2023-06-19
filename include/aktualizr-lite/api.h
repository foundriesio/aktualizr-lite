// Copyright (c) 2021 Foundries.io
// SPDX-License-Identifier: Apache-2.0

#ifndef AKTUALIZR_LITE_API_H_
#define AKTUALIZR_LITE_API_H_

#include <string>

#include <boost/filesystem.hpp>
#include <boost/program_options.hpp>
#include <boost/property_tree/ptree.hpp>

#include "json/json.h"

class Config;
class LiteClient;

/**
 * A high-level representation of a TUF Target in terms applicable to a
 * FoundriesFactory.
 */
class TufTarget {
 public:
  explicit TufTarget() : name_{"unknown"} {}
  TufTarget(std::string name, std::string sha256, int version, Json::Value custom)
      : name_(std::move(name)), sha256_(std::move(sha256)), version_(version), custom_(std::move(custom)) {}

  /**
   * Return the TUF Target name. This is the key in the targets.json key/value
   * singed.metadata dictionary.
   */
  const std::string &Name() const { return name_; }
  /**
   * Return the sha256 OStree hash of the Target.
   */
  const std::string &Sha256Hash() const { return sha256_; }
  /**
   * Return the FoundriesFactory CI build number or in TUF, custom.version.
   */
  int Version() const { return version_; }

  /**
   * Return TUF custom data for a Target.
   */
  const Json::Value &Custom() const { return custom_; }

  /**
   * Is this a known target in the Tuf manifest? There are two common causes
   * to this situation:
   *  1) A device has been re-registered (sql.db got wiped out) and the
   *     /var/sota/installed_versions file is missing. The device might
   *     running the correct target but the system isn't sure.
   *  2) A device might be running a Target from a different tag it's not
   *     configured for. This means the Target isn't present in the targets.json
   *     this device is getting from the device-gateway.
   */
  bool IsUnknown() const { return name_ == "unknown"; }

  /**
   * @brief Compares the given target with the other target
   * @param other - the other target to compare with
   * @return true if the targets match, otherwise false
   */
  bool operator==(const TufTarget &other) const {
    return other.name_ == name_ && other.sha256_ == sha256_ && other.version_ == version_;
  }

 private:
  std::string name_;
  std::string sha256_;
  int version_{-1};
  Json::Value custom_;
};

/**
 * The response from an AkliteClient call to CheckIn()
 */
class CheckInResult {
 public:
  enum class Status {
    Ok = 0,    // check-in was good
    OkCached,  // check-in failed, but locally cached meta-data is still valid
    Failed,    // check-in failed and there's no valid local meta-data
  };
  CheckInResult(Status status, std::string primary_hwid, std::vector<TufTarget> targets)
      : status(status), primary_hwid_(std::move(primary_hwid)), targets_(std::move(targets)) {}
  Status status;
  const std::vector<TufTarget> &Targets() const { return targets_; }
  /**
   * If no hwid is specified, this method will return the latest target for
   * the primary.
   */
  TufTarget GetLatest(std::string hwid = "") const;

 private:
  std::string primary_hwid_;
  std::vector<TufTarget> targets_;
};

/**
 * The response from an AkliteClient call to Install
 */
class InstallResult {
 public:
  enum class Status {
    Ok = 0,
    OkBootFwNeedsCompletion,
    NeedsCompletion,
    BootFwNeedsCompletion,
    Failed,
    DownloadFailed,

  };
  Status status;
  std::string description;

  // NOLINTNEXTLINE(hicpp-explicit-conversions,google-explicit-constructor)
  operator bool() const {
    return status == Status::Ok || status == Status::OkBootFwNeedsCompletion || status == Status::NeedsCompletion;
  }
};

/**
 * The response from an AkliteClient call to Download()
 */
class DownloadResult {
 public:
  enum class Status {
    Ok = 0,
    DownloadFailed,
    VerificationFailed,
    DownloadFailed_NoSpace,
  };
  Status status;
  std::string description;
  std::string destination_path;

  // NOLINTNEXTLINE(hicpp-explicit-conversions,google-explicit-constructor)
  operator bool() const { return status == Status::Ok; }
  bool noSpace() const { return status == Status::DownloadFailed_NoSpace; }
};

std::ostream &operator<<(std::ostream &os, const InstallResult &res);
std::ostream &operator<<(std::ostream &os, const DownloadResult &res);

class InstallContext {
 public:
  InstallContext(const InstallContext &) = delete;
  InstallContext(InstallContext &&) = delete;
  InstallContext &operator=(const InstallContext &) = delete;
  InstallContext &operator=(InstallContext &&) = delete;

  virtual ~InstallContext() = default;
  virtual DownloadResult Download() = 0;
  virtual InstallResult Install() = 0;
  virtual std::string GetCorrelationId() = 0;

  enum class SecondaryEvent {
    DownloadStarted,
    DownloadFailed,
    DownloadCompleted,

    InstallStarted,
    InstallNeedsCompletion,
    InstallCompleted,
    InstallFailed,
  };

  virtual void QueueEvent(std::string ecu_serial, SecondaryEvent event, std::string details) = 0;

 protected:
  InstallContext() = default;
};

struct SecondaryEcu {
  SecondaryEcu(std::string serial, std::string hwid, std::string target_name)
      : serial(std::move(serial)), hwid(std::move(hwid)), target_name(std::move(target_name)) {}
  std::string serial;
  std::string hwid;
  std::string target_name;
};

/**
 * The response from an AkliteClient call to GetDevice
 */
class DeviceResult {
 public:
  enum class Status {
    Ok = 0,
    Failed,
  };
  Status status;
  std::string name;
  std::string factory;
  std::string owner;
  std::string repo_id;
};

/**
 * AkliteClient provides an easy-to-use API for users wanting to customize
 * the behavior of aktualizr-lite.
 */
class AkliteClient {
 public:
  /**
   * Construct a client instance pulling in config files from the given
   * locations. ex:
   *
   *   AkliteClient c(AkliteClient::CONFIG_DIRS)
   *
   * @param config_dirs The list of files/directories to parse sota toml from.
   * @param read_only Run this client in a read-write mode (can do updates)
   * @param finalize Complete/finalize a pending installation in the ctor scope
   */
  explicit AkliteClient(const std::vector<boost::filesystem::path> &config_dirs, bool read_only = false,
                        bool finalize = true);
  /**
   * Construct a client instance with configuration generated from command line
   * arguments.
   * @param cmdline_args The map of commandline arguments.
   * @param read_only Run this client in a read-write mode (can do updates)
   * @param finalize Complete/finalize a pending installation in the ctor scope
   */
  explicit AkliteClient(const boost::program_options::variables_map &cmdline_args, bool read_only = false,
                        bool finalize = true);
  /**
   * Used for unit-testing purposes.
   */
  explicit AkliteClient(std::shared_ptr<LiteClient> client);

  ~AkliteClient();

  AkliteClient(const AkliteClient &) = delete;
  AkliteClient(AkliteClient &&) = delete;
  AkliteClient &operator=(const AkliteClient &) = delete;
  AkliteClient &operator=(AkliteClient &&) = delete;

  /**
   * @brief Checks whether there is ongoing installation
   *
   * Checks whether there is pending installation that has to be completed.
   * To complete installation a device should be rebooted and/or `CompleteInstallation()` called.
   * @return true if there is ongoing installation, otherwise false
   */
  bool IsInstallationInProgress() const;

  /**
   * @brief Returns a pending Target if any
   *
   * Checks whether there is ongoing installation to be completed and returns corresponding Target.
   *
   * @return Pending Target, or "unknown" Target if there is no pending Target
   */
  TufTarget GetPendingTarget() const;

  /**
   * This method can be run at start up to ensure the correct compose apps
   * are running in the event the device's configured list of apps has
   * changed. This method returns nullptr if the apps are in sync. Otherwise
   * an InstallContext is returned that may be called to synchronize the
   * apps.
   */
  std::unique_ptr<InstallContext> CheckAppsInSync() const;

  /**
   * Performs a "check-in" with the device-gateway. A check-in consists of:
   *  1) Report sota.toml. Only do once.
   *  2) Report network info. Only done once unless it changes aftert startup
   *  3) Report hardware info. Only done once.
   *  4) Report Apps state, if Compose App package manager is used
   *  5) ask device-gateway for a new root.json - normally a 404.
   *  6) ask device-gateway for timestamp and snapshot metadata.
   *  7) pull down a new targets.json if needed
   */
  CheckInResult CheckIn() const;

  /**
   * Return the active aktualizr-lite configuration.
   */
  boost::property_tree::ptree GetConfig() const;

  /**
   * Return the Target currently running on the system.
   */
  TufTarget GetCurrent() const;

  /**
   * Find the Target to rollback to in the event the current target wasn't
   * able to start it's Apps after rebooting from an ostree change. This
   * situation is only possible when `pacman.create_containers_before_reboot = 0`.
   */
  TufTarget GetRollbackTarget() const;

  /**
   * Check in with device-gateway to get server managed information about
   * the device.
   */
  DeviceResult GetDevice() const;

  /**
   * Return the device's UUID as defined in the x509 client certificate's CN
   */
  std::string GetDeviceID() const;

  /**
   * Create an InstallContext object to help drive an update.
   */
  std::unique_ptr<InstallContext> Installer(const TufTarget &t, std::string reason = "",
                                            std::string correlation_id = "") const;

  /**
   * @brief Complete a pending installation
   *
   * Runs functionality required to complete/finalize installation after a device reboot:
   * 1) Checks whether a device is booted on the updated ostree-based rootfs.
   * 2) Starts the updated Apps if the boot on the updated rootfs is successful.
   * If #1 or #2 is not successful then marks the given Target as "failing" Target,
   * and returns InstallResult::Failed error.
   *
   * @return Returns:
   *  - InstallResult::Ok on successful installation completion
   *  - InstallResult::OkBootFwNeedsCompletion on successful installation completion; boot fw was updated and require
   * reboot to confirm the update.
   *  - InstallResult::NeedsCompletion if a device was not rebooted after installation
   *  - InstallResult::Failed on failure (see above)
   */
  InstallResult CompleteInstallation();

  /**
   * Check if the Target has been installed but failed to boot. This would
   * make this be considered a "rollback target" and one we shouldn't consider
   * installing.
   */
  bool IsRollback(const TufTarget &t) const;

  /**
   * Set the secondary ECUs managed by this device. Will update the status of
   * the ECUs on the device-gateway and instruct the CheckIn method to also
   * look for targets with the given hardware ids.
   */
  InstallResult SetSecondaries(const std::vector<SecondaryEcu> &ecus);

  /**
   * Default files/paths to search for sota toml when configuration client.
   */
  static const std::vector<boost::filesystem::path> CONFIG_DIRS;

 private:
  void Init(Config &config, bool finalize = true);

  bool read_only_{false};
  std::shared_ptr<LiteClient> client_;
  std::vector<std::string> secondary_hwids_;
  mutable bool configUploaded_{false};
};

#endif  // AKTUALIZR_LITE_API_H_
