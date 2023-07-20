#include "yaml2json.h"

#include <filesystem>

#include <logging/logging.h>
#include "utilities/utils.h"

Yaml2Json::Yaml2Json(const std::string& yaml) {
  if (!std::filesystem::exists(yaml)) {
    throw std::invalid_argument("The specified `yaml` file is not found: " + yaml);
  }
  std::string cmd = "/usr/bin/fy-tool --mode json " + yaml;
  std::string data;
  // Parse the input yaml file
  if (Utils::shell(cmd, &data, true) != EXIT_SUCCESS) {
    throw std::invalid_argument("Failed to parse the input `yaml` file; path: " + yaml + ", err: " + data);
  }
  // Parse the resultant json representation of the input yaml file
  try {
    std::istringstream sin(data);
    sin >> root_;
  } catch (const std::exception& exc) {
    throw std::invalid_argument("Failed to parse the json representation of the input `yaml` file; path: " + yaml +
                                ", err: " + exc.what());
  }
}
