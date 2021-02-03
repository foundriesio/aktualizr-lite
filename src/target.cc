#include "logging/logging.h"

#include "target.h"

bool Target::hasTags(const Uptane::Target& t, const std::vector<std::string>& config_tags) {
  if (!config_tags.empty()) {
    auto tags = t.custom_data()["tags"];
    for (Json::ValueIterator i = tags.begin(); i != tags.end(); ++i) {
      auto tag = (*i).asString();
      if (std::find(config_tags.begin(), config_tags.end(), tag) != config_tags.end()) {
        return true;
      }
    }
    return false;
  }
  return true;
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

bool Target::isForcedTarget(const Uptane::Target& target) {
  return target.custom_data().isMember("update_type") && "force" == target.custom_data()["update_type"].asString();
}

void Target::setForcedUpdate(Uptane::Target& target) {
  Json::Value custom_data = target.custom_data();
  custom_data["update_type"] = "force";
  target.updateCustom(custom_data);
}

void Target::unsetForcedUpdate(Uptane::Target& target) {
  Json::Value custom_data = target.custom_data();
  custom_data.removeMember("update_type");
  target.updateCustom(custom_data);
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
