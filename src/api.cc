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

boost::property_tree::ptree AkliteClient::GetConfig() const {
  std::stringstream ss;
  ss << client_->config;

  boost::property_tree::ptree pt;
  boost::property_tree::ini_parser::read_ini(ss, pt);
  return pt;
}
