#include "client.h"

#include <boost/process.hpp>

#include "exec.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace bp = boost::process;

namespace containerd {

Client::Client(std::string nerdctl_path) : nerdctl_{std::move(nerdctl_path)} {}

void Client::getContainers(Json::Value& root) {
  Json::Value ctrs{Json::arrayValue};
  const std::string cmd{nerdctl_ + " ps -a --format json"};

  bp::ipstream is;
  bp::child c(cmd, bp::std_out > is);

  std::string ctr_json;
  while (c.running() && std::getline(is, ctr_json) && !ctr_json.empty()) {
    ctrs.append(Utils::parseJSON(ctr_json));
  }
  // TODO wait with timeout and make sure that exit code is verified
  c.wait();
  root = ctrs;
}

std::tuple<bool, std::string> Client::getContainerState(const Json::Value& root, const std::string& app,
                                                        const std::string& service, const std::string& hash) const {
  for (Json::ValueConstIterator ii = root.begin(); ii != root.end(); ++ii) {
    Json::Value val = *ii;
    if (val["Labels"]["com.docker.compose.project"].asString() == app) {
      if (val["Labels"]["com.docker.compose.service"].asString() == service) {
        if (val["Labels"]["io.compose-spec.config-hash"].asString() == hash) {
          return {true, val["State"]["Status"].asString()};
        }
      }
    }
  }
  return {false, ""};
}

std::string Client::getContainerLogs(const std::string& /*id*/, int /*tail*/) {
  // TODO
  throw std::runtime_error("Log fetching is not implemented for containerd");
}

const Json::Value& Client::engineInfo() const {
  // It's needed only for restorable Apps
  throw std::runtime_error("Engine info fetching is not implemented for containerd");
}
const std::string& Client::arch() const {
  // It's needed only for restorable Apps
  throw std::runtime_error("Arch obtaining is not implemented for containerd");
}

void Client::pruneImages() {
  // https://github.com/containerd/nerdctl/issues/648
  // Allegedly the prune cmd is supported in nerdctl v0.22.0, currently, LmP runs 0.18.0
  // Also, I tried v0.22.0 and it doesn't work for me...
  LOG_ERROR << "Image pruning is not supported in nerdctl";
}

void Client::pruneContainers() {
  // https://github.com/containerd/nerdctl/issues/648
  // Allegedly the prune cmd is supported in nerdctl v0.22.0, currently, LmP runs 0.18.0
  // Also, I tried v0.22.0 and it doesn't work for me...
  LOG_ERROR << "Container pruning is not supported in nerdctl";
}

void Client::loadImage(const std::string& image_uri, const Json::Value& load_manifest) {
  (void)image_uri;
  (void)load_manifest;
  LOG_ERROR << "Image loading is not implemented";
}

Json::Value Client::getRunningApps(const std::function<void(const std::string&, Json::Value&)>& /* ext_func */) {
  Json::Value apps;
  Json::Value containers;

  getContainers(containers);

  for (Json::ValueIterator ii = containers.begin(); ii != containers.end(); ++ii) {
    Json::Value val = *ii;
    std::string app_name = val["Labels"]["com.docker.compose.project"].asString();
    if (app_name.empty()) {
      continue;
    }

    std::string state = val["State"]["Status"].asString();
    if (state == "stopped") {
      state = "exited";
    }
    std::string status = val["Status"].asString();

    Json::Value service_attributes;
    service_attributes["name"] = val["Labels"]["com.docker.compose.service"].asString();
    service_attributes["hash"] = val["Labels"]["io.compose-spec.config-hash"].asString();
    service_attributes["image"] = val["Image"].asString();
    service_attributes["state"] = state;
    service_attributes["status"] = status;

    // running, created, stopped, paused, pausing, unknown
    service_attributes["health"] = "healthy";
    if (state == "unknown" || (state == "exited" && val["State"]["ExitStatus"].asInt() != 0)) {
      service_attributes["health"] = "unhealthy";
    }

    // TODO: logs
    //    if (service_attributes["health"] != "healthy") {
    //      service_attributes["logs"] = getContainerLogs(val["Id"].asString());
    //    }

    apps[app_name]["services"].append(service_attributes);
  }
  return apps;
}

}  // namespace containerd
