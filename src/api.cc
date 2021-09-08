#include "aktualizr-lite/api.h"

#include "helpers.h"
#include "libaktualizr/config.h"
#include "liteclient.h"

std::vector<boost::filesystem::path> AkliteClient::CONFIG_DIRS = {"/usr/lib/sota/conf.d", "/var/sota/sota.toml",
                                                                  "/etc/sota/conf.d/"};

AkliteClient::AkliteClient(const std::vector<boost::filesystem::path>& config_dirs) {
  Config config(config_dirs);
  config.telemetry.report_network = !config.tls.server.empty();
  config.telemetry.report_config = !config.tls.server.empty();
  client_ = std::make_unique<LiteClient>(config, nullptr);
}

static bool compareTargets(const TufTarget& a, const TufTarget& b) { return a.Version() < b.Version(); }

CheckInResult AkliteClient::CheckIn() const {
  if (!configUploaded_) {
    client_->reportAktualizrConfiguration();
    configUploaded_ = true;
  }
  client_->reportNetworkInfo();
  client_->reportHwInfo();

  auto status = CheckInResult::Status::Ok;
  Uptane::HardwareIdentifier hwidToFind(client_->config.provision.primary_ecu_hardware_id);

  LOG_INFO << "Refreshing Targets metadata";
  const auto rc = client_->updateImageMeta();
  if (!std::get<0>(rc)) {
    LOG_WARNING << "Unable to update latest metadata, using local copy: " << std::get<1>(rc);
    if (!client_->checkImageMetaOffline()) {
      LOG_ERROR << "Unable to use local copy of TUF data";
      return CheckInResult(CheckInResult::Status::Failed, {});
    }
    status = CheckInResult::Status::OkCached;
  }

  std::vector<TufTarget> targets;
  for (const auto& t : client_->allTargets()) {
    int ver = 0;
    try {
      ver = std::stoi(t.custom_version(), nullptr, 0);
    } catch (const std::invalid_argument& exc) {
      LOG_ERROR << "Invalid version number format: " << t.custom_version();
      ver = -1;
    }
    if (!target_has_tags(t, client_->tags)) {
      continue;
    }
    for (const auto& it : t.hardwareIds()) {
      if (it == hwidToFind) {
        targets.emplace_back(t.filename(), t.sha256Hash(), ver, t.custom_data());
        break;
      }
    }
  }

  std::sort(targets.begin(), targets.end(), compareTargets);
  return CheckInResult(status, targets);
}

boost::property_tree::ptree AkliteClient::GetConfig() const {
  std::stringstream ss;
  ss << client_->config;

  boost::property_tree::ptree pt;
  boost::property_tree::ini_parser::read_ini(ss, pt);
  return pt;
}

TufTarget AkliteClient::GetCurrent() const {
  auto current = client_->getCurrent();
  int ver = -1;
  try {
    ver = std::stoi(current.custom_version(), nullptr, 0);
  } catch (const std::invalid_argument& exc) {
    LOG_ERROR << "Invalid version number format: " << current.custom_version();
  }
  return TufTarget(current.filename(), current.sha256Hash(), ver, current.custom_data());
}

bool AkliteClient::IsRollback(const TufTarget& t) const {
  std::vector<Uptane::Target> known_but_not_installed_versions;
  get_known_but_not_installed_versions(*client_, known_but_not_installed_versions);

  Json::Value target_json;
  target_json["hashes"]["sha256"] = t.Sha256Hash();
  target_json["custom"]["targetFormat"] = "OSTREE";
  target_json["length"] = 0;
  Uptane::Target target(t.Name(), target_json);

  return known_local_target(*client_, target, known_but_not_installed_versions);
}
