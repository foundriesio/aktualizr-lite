// Copyright (c) 2021 Foundries.io
// SPDX-License-Identifier: Apache-2.0

#ifndef AKTUALIZR_LITE_API_H_
#define AKTUALIZR_LITE_API_H_

#include <string>

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "json/json.h"

class LiteClient;

/**
 * A high-level representation of a TUF Target in terms applicable to a
 * FoundriesFactory.
 */
class TufTarget {
 public:
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

 private:
  std::string name_;
  std::string sha256_;
  int version_;
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
    NeedsCompletion,
    Failed,
  };
  Status status;
  std::string description;
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
  };
  Status status;
  std::string description;
};

std::ostream &operator<<(std::ostream &os, const InstallResult &res);
std::ostream &operator<<(std::ostream &os, const DownloadResult &res);

class InstallContext {
 public:
  virtual ~InstallContext() = default;
  virtual DownloadResult Download() = 0;
  virtual InstallResult Install() = 0;
};

struct SecondaryEcu {
  SecondaryEcu(std::string serial, std::string hwid, std::string target_name)
      : serial(std::move(serial)), hwid(std::move(hwid)), target_name(std::move(target_name)) {}
  std::string serial;
  std::string hwid;
  std::string target_name;
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
   */
  AkliteClient(const std::vector<boost::filesystem::path> &config_dirs);
  /**
   * Used for unit-testing purposes.
   */
  AkliteClient(std::shared_ptr<LiteClient> client) : client_(client) {}

  /**
   * Performs a "check-in" with the device-gateway. A check-in consists of:
   *  1) Report sota.toml. Only do once.
   *  2) Report network info. Only done once unless it changes aftert startup
   *  3) Report hardware info. Only done once.
   *  4) ask device-gateway for a new root.json - normally a 404.
   *  5) ask device-gateway for timestamp and snapshot metadata.
   *  6) pull down a new targets.json if needed
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
   * Create an InstallContext object to help drive an update.
   */
  std::unique_ptr<InstallContext> Installer(const TufTarget &t, std::string reason = "") const;

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
  static std::vector<boost::filesystem::path> CONFIG_DIRS;

 private:
  std::shared_ptr<LiteClient> client_;
  std::vector<std::string> secondary_hwids_;
  mutable bool configUploaded_{false};
};

#endif  // AKTUALIZR_LITE_API_H_
