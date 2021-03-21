#ifndef AKTUALIZR_LITE_COMPOSEINFO_H
#define AKTUALIZR_LITE_COMPOSEINFO_H

#include <json/json.h>
#include <string>
#include <vector>
#include "yaml2json.h"
namespace Docker {

class ComposeInfo {
 public:
  ComposeInfo(const std::string& yaml);
  std::vector<Json::Value> getServices();
  std::string getImage(const Json::Value& service);
  std::string getHash(const Json::Value& service);

 private:
  Yaml2Json json_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_COMPOSEINFO_H
