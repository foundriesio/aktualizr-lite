#include "helpers.h"

#include "composeappmanager.h"

void log_info_target(const std::string& prefix, const Config& config, const Uptane::Target& t) {
  auto name = t.filename();
  if (t.custom_version().length() > 0) {
    name = t.custom_version();
  }
  LOG_INFO << prefix + name << "\tsha256:" << t.sha256Hash();

  if (config.pacman.type == ComposeAppManager::Name) {
    bool shown = false;
    auto config_apps = ComposeAppManager::Config(config.pacman).apps;
    auto bundles = t.custom_data()["docker_compose_apps"];
    for (Json::ValueIterator i = bundles.begin(); i != bundles.end(); ++i) {
      if (!shown) {
        shown = true;
        LOG_INFO << "\tDocker Compose Apps:";
      }
      if ((*i).isObject() && (*i).isMember("uri")) {
        const auto& app = i.key().asString();
        std::string app_status =
            (!config_apps || (*config_apps).end() != std::find((*config_apps).begin(), (*config_apps).end(), app))
                ? "on"
                : "off";
        LOG_INFO << "\t" << app_status << ": " << app << " -> " << (*i)["uri"].asString();
      } else {
        LOG_ERROR << "\t\tInvalid custom data for docker_compose_apps: " << i.key().asString();
      }
    }
  }
}
