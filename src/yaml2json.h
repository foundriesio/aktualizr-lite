#ifndef AKTUALIZR_LITE_YAML2JSON_H
#define AKTUALIZR_LITE_YAML2JSON_H

#include <json/json.h>
#include <string>

class Yaml2Json {
 public:
  explicit Yaml2Json(const std::string& yaml);
  Json::Value root_;
};

#endif  // AKTUALIZR_LITE_YAML2JSON_H
