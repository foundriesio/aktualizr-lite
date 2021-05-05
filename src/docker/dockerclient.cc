#include "dockerclient.h"
#include <boost/format.hpp>
#include "http/httpclient.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace Docker {

DockerClient::HttpClientFactory DockerClient::DefaultHttpClientFactory = []() {
  return std::make_shared<HttpClient>("/var/run/docker.sock");
};

DockerClient::DockerClient(std::shared_ptr<HttpInterface> http_client) : http_client_{std::move(http_client)} {}

bool DockerClient::isRunning(const std::string& app, const std::string& service, const std::string& hash) {
  Json::Value root;
  refresh(root);
  for (Json::ValueIterator ii = root.begin(); ii != root.end(); ++ii) {
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

std::string DockerClient::runningApps() {
  std::string runningApps;
  Json::Value root;
  refresh(root);
  for (Json::ValueIterator ii = root.begin(); ii != root.end(); ++ii) {
    Json::Value val = *ii;
    std::string app = val["Labels"]["com.docker.compose.project"].asString();
    std::string service = val["Labels"]["com.docker.compose.service"].asString();
    std::string hash = val["Labels"]["io.compose-spec.config-hash"].asString();

    boost::format format("App(%s) Service(%s %s)\n");
    std::string line = boost::str(format % app % service % hash);
    runningApps += line;
  }
  return runningApps;
}

void DockerClient::refresh(Json::Value& root) {
  std::string cmd;
  if (!http_client_) {
    cmd = "/usr/bin/curl --unix-socket /var/run/docker.sock http://localhost/containers/json";
    std::string data;
    if (Utils::shell(cmd, &data, false) == EXIT_SUCCESS) {
      std::istringstream sin(data);
      sin >> root;
    }
  } else {
    cmd = "http://localhost/containers/json";
    auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
    if (resp.isOk()) {
      root = resp.getJson();
    }
  }
  if (!root) {
    // check if the `root` json is initialized, not `empty()` since dockerd can return 200/OK with
    // empty json `[]`, which is not exceptional situation and means zero containers are running
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
}

}  // namespace Docker
