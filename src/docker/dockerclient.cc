#include "dockerclient.h"
#include <logging/logging.h>
#include "http/httpclient.h"
#include "utilities/utils.h"

namespace Docker {

DockerClient::DockerClient(const std::string& app, bool curl) {
  std::string cmd;
  if (curl) {
    cmd = "/usr/bin/curl --unix-socket /var/run/docker.sock http://localhost/containers/json?" + app;
    std::string data;
    if (Utils::shell(cmd, &data, false) == EXIT_SUCCESS) {
      std::istringstream sin(data);
      sin >> root_;
    }
  } else {
    cmd = "http://localhost/containers/json?" + app;
    HttpClient client("/var/run/docker.sock");
    auto resp = client.get(cmd, HttpInterface::kNoLimit);
    if (resp.isOk()) {
      root_ = resp.getJson();
    }
  }
  if (root_.empty()) {
    throw std::runtime_error(cmd.c_str());
  }
}

bool DockerClient::serviceRunning(std::string& service, std::string& hash) {
  for (Json::ValueIterator ii = root_.begin(); ii != root_.end(); ++ii) {
    Json::Value val = *ii;
    if (val["Labels"]["com.docker.compose.service"].asString() == service) {
      if (val["Labels"]["io.compose-spec.config-hash"].asString() == hash) {
        return true;
      }
    }
  }
  return false;
}

}  // namespace Docker
