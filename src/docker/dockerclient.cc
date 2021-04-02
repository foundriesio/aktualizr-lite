#include "dockerclient.h"

#include "http/httpclient.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace Docker {

DockerClient::HttpClientFactory DockerClient::DefaultHttpClientFactory = []() {
  return std::make_shared<HttpClient>("/var/run/docker.sock");
};

DockerClient::DockerClient(std::shared_ptr<HttpInterface> http_client) : http_client_{std::move(http_client)} {}

bool DockerClient::serviceRunning(const std::string& app, const std::string& service, const std::string& hash) {
  updateContainerStatus();
  for (Json::ValueIterator ii = root_.begin(); ii != root_.end(); ++ii) {
    Json::Value val = *ii;
    if (val["Labels"]["com.docker.compose.project"].asString() == app) {
      if (val["Labels"]["com.docker.compose.service"].asString() == service) {
        if (val["Labels"]["io.compose-spec.config-hash"].asString() == hash) {
          return true;
        }
      }
    }
  }
  return false;
}

void DockerClient::updateContainerStatus(bool curl) {
  std::string cmd;
  if (curl) {
    cmd = "/usr/bin/curl --unix-socket /var/run/docker.sock http://localhost/containers/json";
    std::string data;
    if (Utils::shell(cmd, &data, false) == EXIT_SUCCESS) {
      std::istringstream sin(data);
      sin >> root_;
    }
  } else {
    cmd = "http://localhost/containers/json";
    auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
    if (resp.isOk()) {
      root_ = resp.getJson();
    }
  }
  if (!root_) {
    // check if the `root_` json is initialized, not `empty()` since dockerd can return 200/OK with
    // empty json `[]`, which is not exceptional situation and means zero containers are running
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
}

}  // namespace Docker
