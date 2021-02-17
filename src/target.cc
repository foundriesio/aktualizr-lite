#include "target.h"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

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

Target::Apps Target::targetApps(const Uptane::Target& target, boost::optional<std::vector<std::string>> shortlist) {
  if (!target.IsValid()) {
    throw std::runtime_error("Failed to get target apps: the specified Target is invalid");
  }

  const auto target_custom_data = target.custom_data();
  if (target_custom_data.isNull() || !target_custom_data.isObject()) {
    throw std::runtime_error("Failed to get target apps: the specified Target doesn't include a custom data: " +
                             target.filename());
  }

  Apps apps;

  if (!target_custom_data.isMember(ComposeAppField)) {
    // throw std::runtime_error("Failed to get target apps: the specified Target doesn't include the compose app field:
    // " + target.filename());
    return apps;
  }

  const auto target_apps = target_custom_data[ComposeAppField];

  for (Json::ValueConstIterator ii = target_apps.begin(); ii != target_apps.end(); ++ii) {
    if ((*ii).isNull() || !(*ii).isObject() || !(*ii).isMember("uri")) {
      throw std::runtime_error("Failed to get target apps: the specified Target has an invalid app map: " +
                               target_apps.toStyledString());
    }

    const auto& app_name = ii.key().asString();

    if (!!shortlist && (*shortlist).end() == std::find((*shortlist).begin(), (*shortlist).end(), app_name)) {
      continue;
    }

    apps.emplace_back(ii.key().asString(), (*ii)["uri"].asString());
  }

  return apps;
}

void Target::shortlistTargetApps(Uptane::Target& target, std::vector<std::string> shortlist) {
  if (!target.IsValid()) {
    throw std::runtime_error("Failed to get target apps: the specified Target is invalid");
  }

  auto target_custom_data = target.custom_data();
  if (target_custom_data.isNull() || !target_custom_data.isObject()) {
    throw std::runtime_error("Failed to get target apps: the specified Target doesn't include a custom data: " +
                             target.filename());
  }

  if (!target_custom_data.isMember(ComposeAppField)) {
    // throw std::runtime_error("Failed to get target apps: the specified Target doesn't include the compose app field:
    // " + target.filename());
    return;
  }

  const auto target_apps = target_custom_data[ComposeAppField];
  // target_custom_data.removeMember(ComposeAppField);

  for (Json::ValueConstIterator ii = target_apps.begin(); ii != target_apps.end(); ++ii) {
    if ((*ii).isNull() || !(*ii).isObject() || !(*ii).isMember("uri")) {
      throw std::runtime_error("Failed to get target apps: the specified Target has an invalid app map: " +
                               target_apps.toStyledString());
    }

    const auto& app_name = ii.key().asString();

    if (shortlist.end() != std::find(shortlist.begin(), shortlist.end(), app_name)) {
      continue;
    }

    target_custom_data[ComposeAppField].removeMember(app_name);
  }

  target.updateCustom(target_custom_data);
}

Uptane::Target Target::subtractCurrentApps(Uptane::Target& target, Uptane::Target& current) {
  Uptane::Target result = target;

  if (!target.IsValid()) {
    throw std::runtime_error("Failed to get target apps: the specified Target is invalid");
  }

  auto target_custom_data = target.custom_data();
  if (target_custom_data.isNull() || !target_custom_data.isObject()) {
    throw std::runtime_error("Failed to get target apps: the specified Target doesn't include a custom data: " +
                             target.filename());
  }

  if (!target_custom_data.isMember(ComposeAppField)) {
    // throw std::runtime_error("Failed to get target apps: the specified Target doesn't include the compose app field:
    // " + target.filename());
    return result;
  }

  const auto target_apps = target_custom_data[ComposeAppField];
  auto result_custom = result.custom_data();
  auto current_custom = current.custom_data();
  auto current_apps = current.custom_data()[ComposeAppField];

  for (Json::ValueConstIterator ii = target_apps.begin(); ii != target_apps.end(); ++ii) {
    if ((*ii).isNull() || !(*ii).isObject() || !(*ii).isMember("uri")) {
      throw std::runtime_error("Failed to get target apps: the specified Target has an invalid app map: " +
                               target_apps.toStyledString());
    }

    const auto& app_name = ii.key().asString();
    if (current_apps.isMember(app_name)) {
      result_custom[ComposeAppField].removeMember(app_name);
    }
  }

  result.updateCustom(result_custom);
  return result;
}
