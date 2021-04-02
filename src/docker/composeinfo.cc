#include "composeinfo.h"
#include <glib.h>
#include "logging/logging.h"
#include "yaml2json.h"

namespace Docker {

ComposeInfo::ComposeInfo(const std::string& yaml) : json_(yaml) {}

std::vector<Json::Value> ComposeInfo::getServices() {
  Json::Value p = json_.root_["services"];
  std::vector<Json::Value> services;
  for (Json::ValueIterator ii = p.begin(); ii != p.end(); ++ii) {
    services.push_back(ii.key());
  }
  return services;
}

std::string ComposeInfo::getImage(const Json::Value& service) {
  return json_.root_["services"][service.asString()]["image"].asString();
}

std::string ComposeInfo::getHash(const Json::Value& service) {
  return json_.root_["services"][service.asString()]["labels"]["io.compose-spec.config-hash"].asString();
}

}  // namespace Docker
