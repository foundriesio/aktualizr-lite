// Copyright (c) 2021 Foundries.io
// SPDX-License-Identifier: Apache-2.0

#include <string>

#include <boost/filesystem.hpp>
#include <boost/property_tree/ptree.hpp>

#include "json/json.h"

class LiteClient;

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
   * Default files/paths to search for sota toml when configuration client.
   */
  static std::vector<boost::filesystem::path> CONFIG_DIRS;

 private:
  std::shared_ptr<LiteClient> client_;
};
