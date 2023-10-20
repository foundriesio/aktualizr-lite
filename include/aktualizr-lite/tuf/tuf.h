// Copyright (c) 2023 Foundries.io
// SPDX-License-Identifier: Apache-2.0

#ifndef AKLITE_TUF_TUF_H_
#define AKLITE_TUF_TUF_H_

#include <string>

#include "json/json.h"

/**
 * This header contains the definition of an EXPERIMENTAL API for accessing TUF
 * functionality. It allows for better isolation between Aktualizr-lite and
 * libaktualizr, and better handling of multiple metadata sources for a same
 * TUF repository (for example, mirrors or pre-fetched metadata files).
 */

namespace aklite::tuf {

/**
 * Interface for a TUF repository metadata source.
 */
class RepoSource {
 public:
  virtual ~RepoSource() = default;
  RepoSource(const RepoSource&) = delete;
  RepoSource(const RepoSource&&) = delete;
  RepoSource& operator=(const RepoSource&) = delete;
  RepoSource& operator=(const RepoSource&&) = delete;

  virtual std::string fetchRoot(int version) = 0;
  virtual std::string fetchTimestamp() = 0;
  virtual std::string fetchSnapshot() = 0;
  virtual std::string fetchTargets() = 0;

 protected:
  RepoSource() = default;
};

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
  const std::string& Name() const { return name_; }
  /**
   * Return the sha256 OStree hash of the Target.
   */
  const std::string& Sha256Hash() const { return sha256_; }
  /**
   * Return the FoundriesFactory CI build number or in TUF, custom.version.
   */
  int Version() const { return version_; }

  /**
   * Return TUF custom data for a Target.
   */
  const Json::Value& Custom() const { return custom_; }

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
  bool operator==(const TufTarget& other) const {
    return other.name_ == name_ && other.sha256_ == sha256_ && other.version_ == version_;
  }

 private:
  std::string name_;
  std::string sha256_;
  int version_{-1};
  Json::Value custom_;
};

/**
 * Interface for a TUF specification engine, handling a single repository,
 * fed through one or more consistent RepoSource instances.
 */
class Repo {
 public:
  virtual ~Repo() = default;
  Repo(const Repo&) = delete;
  Repo(const Repo&&) = delete;
  Repo& operator=(const Repo&) = delete;
  Repo& operator=(const Repo&&) = delete;

  virtual std::vector<TufTarget> GetTargets() = 0;
  virtual void updateMeta(std::shared_ptr<RepoSource> repo_src) = 0;

 protected:
  Repo() = default;
};

}  // namespace aklite::tuf

#endif