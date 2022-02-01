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

std::string Target::appsStr(const Uptane::Target& target,
                            const boost::optional<std::vector<std::string>>& app_shortlist) {
  std::vector<std::string> apps;
  for (const auto& app : Target::Apps(target)) {
    if (!app_shortlist ||
        (*app_shortlist).end() != std::find((*app_shortlist).begin(), (*app_shortlist).end(), app.name)) {
      apps.push_back(app.name);
    }
  }
  return boost::algorithm::join(apps, ",");
}

void Target::log(const std::string& prefix, const Uptane::Target& target,
                 boost::optional<std::vector<std::string>> app_shortlist) {
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
    std::string app_status =
        (!app_shortlist || std::find(app_shortlist->begin(), app_shortlist->end(), app.name) != app_shortlist->end())
            ? "on "
            : "off";
    LOG_INFO << "\t" << app_status << ": " << app.name << " -> " << app.uri;
  }
}

Uptane::Target Target::fromTufTarget(const TufTarget& target) {
  Json::Value target_json;
  target_json["hashes"]["sha256"] = target.Sha256Hash();
  target_json["length"] = 0;
  target_json["custom"] = target.Custom();
  return Uptane::Target{target.Name(), target_json};
}

TufTarget Target::toTufTarget(const Uptane::Target& target) {
  int ver = -1;
  try {
    ver = std::stoi(target.custom_version(), nullptr, 0);
  } catch (const std::invalid_argument& exc) {
    LOG_ERROR << "Invalid version number format: " << exc.what();
  }

  return TufTarget{target.filename(), target.sha256Hash(), ver, target.custom_data()};
}
