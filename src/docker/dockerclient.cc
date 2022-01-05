#include "dockerclient.h"
#include <boost/format.hpp>
#include "http/httpclient.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace Docker {

DockerClient::HttpClientFactory DockerClient::DefaultHttpClientFactory = []() {
  return std::make_shared<HttpClient>("/var/run/docker.sock");
};

DockerClient::DockerClient(std::shared_ptr<HttpInterface> http_client)
    : http_client_{std::move(http_client)},
      engine_info_{getEngineInfo()},
      arch_{engine_info_.get("Arch", Json::Value()).asString()} {}

void DockerClient::getContainers(Json::Value& root) {
  // curl --unix-socket /var/run/docker.sock http://localhost/containers/json?all=1
  const std::string cmd{"http://localhost/containers/json?all=1"};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (resp.isOk()) {
    root = resp.getJson();
  }
  if (!root) {
    // check if the `root` json is initialized, not `empty()` since dockerd can return 200/OK with
    // empty json `[]`, which is not exceptional situation and means zero containers are running
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
}

std::tuple<bool, std::string> DockerClient::getContainerState(const Json::Value& root, const std::string& app,
                                                              const std::string& service, const std::string& hash) {
  for (Json::ValueConstIterator ii = root.begin(); ii != root.end(); ++ii) {
    Json::Value val = *ii;
    if (val["Labels"]["com.docker.compose.project"].asString() == app) {
      if (val["Labels"]["com.docker.compose.service"].asString() == service) {
        if (val["Labels"]["io.compose-spec.config-hash"].asString() == hash) {
          return {true, val["State"].asString()};
        }
      }
    }
  }
  return {false, ""};
}

Json::Value DockerClient::getEngineInfo() {
  Json::Value info;
  const std::string cmd{"http://localhost/version"};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (resp.isOk()) {
    info = resp.getJson();
  }
  if (!info) {
    // check if the `root` json is initialized, not `empty()` since dockerd can return 200/OK with
    // empty json `[]`, which is not exceptional situation and means zero containers are running
    throw std::runtime_error("Request to the dockerd's /version endpoint has failed: " + cmd);
  }
  return info;
}

}  // namespace Docker
