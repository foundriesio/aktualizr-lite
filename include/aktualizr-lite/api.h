// Copyright (c) 2021 Foundries.io
// SPDX-License-Identifier: Apache-2.0

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
   * Return the active aktualizr-lite configuration.
   */
  boost::property_tree::ptree GetConfig() const;

  /**
   * Return the Target currently running on the system.
   */
  TufTarget GetCurrent() const;

  /**
   * Default files/paths to search for sota toml when configuration client.
   */
  static std::vector<boost::filesystem::path> CONFIG_DIRS;

 private:
  std::shared_ptr<LiteClient> client_;
};
