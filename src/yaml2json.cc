#include "yaml2json.h"
#include <logging/logging.h>
#include "utilities/utils.h"

Yaml2Json::Yaml2Json(const std::string& yaml) {
  std::string cmd = "/usr/bin/fy-tool --mode json " + yaml;
  std::string data;
  if (Utils::shell(cmd, &data, true) == EXIT_SUCCESS) {
    std::istringstream sin(data);
    sin >> root_;
  }
  if (root_.empty()) {
    throw std::runtime_error(cmd.c_str());
  }
}
