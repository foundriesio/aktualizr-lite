#include "target.h"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "logging/logging.h"

bool Target::hasTag(const Uptane::Target& target, const std::vector<std::string>& tags) {
  if (tags.empty()) {
    return true;
  }

  const auto target_tags = target.custom_data()[TagField];
  for (Json::ValueConstIterator ii = target_tags.begin(); ii != target_tags.end(); ++ii) {
    auto target_tag = (*ii).asString();
    if (std::find(tags.begin(), tags.end(), target_tag) != tags.end()) {
      return true;
    }
  }
  return false;
}

void Target::setCorrelationID(Uptane::Target& target) {
  std::string id = target.custom_version();
  if (id.empty()) {
    id = target.filename();
  }
  boost::uuids::uuid tmp = boost::uuids::random_generator()();
  target.setCorrelationId(id + "-" + boost::uuids::to_string(tmp));
}

std::string Target::ostreeURI(const Uptane::Target& target) {
  std::string uri;
  auto uri_json = target.custom_data().get("compose-apps-uri", Json::Value(Json::nullValue));
  if (!uri_json.empty()) {
    uri = uri_json.asString();
  }
  return uri;
}

Json::Value Target::appsJson(const Uptane::Target& target) {
  return target.custom_data().get(Target::ComposeAppField, Json::Value(Json::nullValue));
}

void Target::setAppsJson(Uptane::Target& target, const Json::Value& apps_json) {
  Json::Value custom_data = target.custom_data();
  if (!custom_data.isNull()) {
    custom_data[Target::ComposeAppField] = apps_json;
    target.updateCustom(custom_data);
  }
}

void Target::log(const std::string& prefix, const Uptane::Target& target,
                 boost::optional<std::set<std::string>> shortlist) {
  auto name = target.filename();
  if (target.custom_version().length() > 0) {
    name = target.custom_version();
  }
  LOG_INFO << prefix + name << "\tsha256:" << target.sha256Hash();

  bool print_title{true};
  for (const auto& app : Target::Apps(target)) {
    if (print_title) {
      LOG_INFO << "\tDocker Compose Apps:";
      print_title = false;
    }
    std::string app_status = (!shortlist || 0 != shortlist->count(app.name)) ? "on " : "off";
    LOG_INFO << "\t" << app_status << ": " << app.name << " -> " << app.uri;
  }
}
