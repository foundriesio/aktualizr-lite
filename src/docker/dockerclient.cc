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

bool DockerClient::getImages(Json::Value& images) {
  const auto* const cmd = "http://localhost/images/json";
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (!resp.isOk()) {
    return false;
  }

  bool result{false};
  try {
    Json::Value images_all = resp.getJson();
    for (Json::ValueConstIterator ii = images_all.begin(); ii != images_all.end(); ++ii) {
      images[(*ii)["RepoDigests"][0].asString()] = "";
    }
    result = true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to parse received response: " << e.what();
  }
  return result;
}

bool DockerClient::isImagePresent(const Json::Value& images, const std::string& image_url) {
  return images.isMember(image_url);
}

bool DockerClient::getContainers(Json::Value& containers) {
  const auto* const cmd = "http://localhost/containers/json?all=1";
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (!resp.isOk()) {
    return false;
  }

  bool result{false};
  try {
    Json::Value cont_all = resp.getJson();
    for (Json::ValueConstIterator ii = cont_all.begin(); ii != cont_all.end(); ++ii) {
      const auto image_url = (*ii)["Image"].asString();
      containers[image_url]["service"] = (*ii)["Labels"]["com.docker.compose.service"];
      containers[(*ii)["Image"].asString()]["service_hash"] = (*ii)["Labels"].get("io.compose-spec.config-hash", "");
      containers[(*ii)["Image"].asString()]["state"] = (*ii)["State"].asString();
    }
    result = true;
  } catch (const std::exception& e) {
    LOG_ERROR << "Failed to parse received response: " << e.what();
  }
  return result;
}

bool DockerClient::isContainerStarted(const Json::Value& containers, const std::string& image_url,
                                      const std::string& service, const std::string& service_hash) {
  const auto& cont_info = containers.get(image_url, Json::nullValue);
  if (cont_info.empty()) {
    return false;
  }

  return cont_info["service"] == service && cont_info["service_hash"] == service_hash &&
         (cont_info["state"] == "running" || cont_info["state"] == "exited" || cont_info["state"] == "dead");
}

std::string DockerClient::runningApps() {
  std::string runningApps;
  Json::Value root;
  refresh(root);
  for (Json::ValueIterator ii = root.begin(); ii != root.end(); ++ii) {
    Json::Value val = *ii;
    std::string app = val["Labels"]["com.docker.compose.project"].asString();
    std::string image_url = val["Image"].asString();
    std::string cont_state = val["State"].asString();
    std::string service = val["Labels"]["com.docker.compose.service"].asString();
    std::string hash = val["Labels"]["io.compose-spec.config-hash"].asString();

    boost::format format("App(%s) Service(%s %s) Image(%s) Container(%s)\n");
    std::string line = boost::str(format % app % service % hash % image_url % cont_state);
    runningApps += line;
  }
  return runningApps;
}

void DockerClient::refresh(Json::Value& root) {
  std::string cmd;
  if (!http_client_) {
    cmd = "/usr/bin/curl --unix-socket /var/run/docker.sock http://localhost/containers/json?all=1";
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
