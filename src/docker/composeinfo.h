#ifndef AKTUALIZR_LITE_COMPOSE_INFO_H
#define AKTUALIZR_LITE_COMPOSE_INFO_H

#include <json/json.h>
#include <string>
#include <vector>
#include "yaml2json.h"

namespace Docker {

class ComposeInfo {
 public:
  explicit ComposeInfo(const std::string& yaml);
  std::vector<Json::Value> getServices() const;
  std::string getImage(const Json::Value& service) const;
  std::string getHash(const Json::Value& service) const;

 private:
  Yaml2Json json_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_COMPOSE_INFO_H
