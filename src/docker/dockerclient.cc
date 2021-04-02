#include "dockerclient.h"

#include "http/httpclient.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace Docker {

DockerClient::HttpClientFactory DockerClient::DefaultHttpClientFactory = []() {
  return std::make_shared<HttpClient>("/var/run/docker.sock");
};

DockerClient::DockerClient(const std::string& app, bool curl, const HttpClientFactory& http_client_factory) {
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
    std::shared_ptr<HttpInterface> client{http_client_factory()};
    auto resp = client->get(cmd, HttpInterface::kNoLimit);
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
