#include "helpers.h"

#include <boost/uuid/uuid_generators.hpp>
#include <boost/uuid/uuid_io.hpp>

#include "composeappmanager.h"
#include "storage/invstorage.h"

static bool appListChanged(const Json::Value& target_apps, std::vector<std::string>& cfg_apps_in,
                           const boost::filesystem::path& apps_dir) {
  // Did the list of installed versus running apps change:
  std::vector<std::string> found;
  if (boost::filesystem::is_directory(apps_dir)) {
    for (auto& entry : boost::make_iterator_range(boost::filesystem::directory_iterator(apps_dir), {})) {
      if (boost::filesystem::is_directory(entry)) {
        found.emplace_back(entry.path().filename().native());
      }
    }
  }
  // don't take into consideration the apps that are listed in the config but are not present in Target
  // do take into consideration the apps that are found on a file system but are not present in Target
  // since they should be removed, so we need to trigger the installation procedure that will remove them
  auto cfg_apps_filtered_end = cfg_apps_in.end();
  if (!target_apps.isNull()) {
    cfg_apps_filtered_end =
        std::remove_if(cfg_apps_in.begin(), cfg_apps_in.end(),
                       [&target_apps](const std::string& app) { return !target_apps.isMember(app.c_str()); });
  }
  std::vector<std::string> cfg_apps{cfg_apps_in.begin(), cfg_apps_filtered_end};
  std::sort(found.begin(), found.end());
  std::sort(cfg_apps.begin(), cfg_apps.end());
  if (found != cfg_apps) {
    LOG_INFO << "Config change detected: list of apps has changed";
    return true;
  }
  return false;
}

bool LiteClient::composeAppsChanged() const {
  if (config.pacman.type == ComposeAppManager::Name) {
    ComposeAppManager::Config cacfg(config.pacman);
    if (!cacfg.apps) {
      // `compose_apps` is not specified in the config at all
      return false;
    }
    if (appListChanged(getCurrent().custom_data()["docker_compose_apps"], *cacfg.apps, cacfg.apps_root)) {
      return true;
    }

  } else {
    return false;
  }

  return false;
}

void generate_correlation_id(Uptane::Target& t) {
  std::string id = t.custom_version();
  if (id.empty()) {
    id = t.filename();
  }
  boost::uuids::uuid tmp = boost::uuids::random_generator()();
  t.setCorrelationId(id + "-" + boost::uuids::to_string(tmp));
}

bool target_has_tags(const Uptane::Target& t, const std::vector<std::string>& config_tags) {
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

bool known_local_target(LiteClient& client, const Uptane::Target& t,
                        std::vector<Uptane::Target>& known_but_not_installed_versions) {
  bool known_target = false;
  auto current = client.getCurrent();
  boost::optional<Uptane::Target> pending;
  client.storage->loadPrimaryInstalledVersions(nullptr, &pending);

  if (t.sha256Hash() != current.sha256Hash()) {
    std::vector<Uptane::Target>::reverse_iterator it;
    for (it = known_but_not_installed_versions.rbegin(); it != known_but_not_installed_versions.rend(); it++) {
      if (it->sha256Hash() == t.sha256Hash()) {
        // Make sure installed version is not what is currently pending
        if ((pending != boost::none) && (it->sha256Hash() == pending->sha256Hash())) {
          continue;
        }
        LOG_INFO << "Target sha256Hash " << t.sha256Hash() << " known locally (rollback?), skipping";
        known_target = true;
        break;
      }
    }
  }
  return known_target;
}

void get_known_but_not_installed_versions(LiteClient& client,
                                          std::vector<Uptane::Target>& known_but_not_installed_versions) {
  std::vector<Uptane::Target> known_versions;
  client.storage->loadPrimaryInstallationLog(&known_versions, false);

  std::vector<Uptane::Target> installed_versions;
  client.storage->loadPrimaryInstallationLog(&installed_versions, true);

  for (const auto& t : known_versions) {
    if (installed_versions.end() ==
        std::find_if(installed_versions.begin(), installed_versions.end(),
                     [&t](const Uptane::Target& t1) { return t.filename() == t1.filename(); })) {
      // known but never successfully installed version
      known_but_not_installed_versions.push_back(t);
    }
  }
}
