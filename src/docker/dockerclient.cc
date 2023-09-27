#include "dockerclient.h"

#include <archive.h>
#include <archive_entry.h>
#include <boost/format.hpp>
#include <boost/lexical_cast.hpp>
#include <boost/process.hpp>

#include "http/httpclient.h"
#include "logging/logging.h"
#include "utilities/utils.h"

namespace Docker {

const DockerClient::HttpClientFactory DockerClient::DefaultHttpClientFactory = [](const std::string& docker_host_in) {
  std::string docker_host{docker_host_in};
  auto env{boost::this_process::environment()};
  if (env.end() != env.find("DOCKER_HOST")) {
    docker_host = env.get("DOCKER_HOST");
  }
  static const std::string docker_host_prefix{"unix://"};
  const auto find_res = docker_host.find_first_of(docker_host_prefix);
  if (find_res != 0) {
    throw std::invalid_argument("Invalid docker host value, must start with unix:// : " + docker_host);
  }

  const auto socket{docker_host.substr(docker_host_prefix.size())};
  auto c{std::make_shared<HttpClient>(socket)};
  // Set a timeout for the overall request processing:
  // "the maximum time in milliseconds that you allow the entire transfer operation to take".
  int64_t timeout_ms{1000 * 60}; /* by default 1m timeout */
  if (1 == env.count("COMPOSE_HTTP_TIMEOUT")) {
    std::string timeout_str;
    try {
      timeout_str = env.get("COMPOSE_HTTP_TIMEOUT");
      const auto timeout_s{boost::lexical_cast<int64_t>(timeout_str)};
      timeout_ms = timeout_s * 1000;
      LOG_DEBUG << "Docker client: setting the timeout defined by `COMPOSE_HTTP_TIMEOUT` env variable: " << timeout_str;
    } catch (const std::exception& exc) {
      LOG_ERROR << "Invalid timeout value set by `COMPOSE_HTTP_TIMEOUT`; value: " << timeout_str
                << ", err: " << exc.what() << "; applying the default value: 60s";
    }
  }
  c->timeout(timeout_ms);
  return c;
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
                                                              const std::string& service,
                                                              const std::string& hash) const {
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

Json::Value DockerClient::getContainerInfo(const std::string& id) {
  const std::string cmd{"http://localhost/containers/" + id + "/json"};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (!resp.isOk()) {
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
  return resp.getJson();
}

std::string DockerClient::getContainerLogs(const std::string& id, int tail) {
  const std::string cmd{"http://localhost/containers/" + id + "/logs?stderr=1&tail=" + std::to_string(tail)};
  auto resp = http_client_->get(cmd, HttpInterface::kNoLimit);
  if (!resp.isOk()) {
    throw std::runtime_error("Request to dockerd has failed: " + cmd);
  }
  return resp.body;
}

Json::Value DockerClient::getRunningApps(const std::function<void(const std::string&, Json::Value&)>& ext_func) {
  Json::Value apps;
  Json::Value containers;
  getContainers(containers);

  for (Json::ValueIterator ii = containers.begin(); ii != containers.end(); ++ii) {
    Json::Value val = *ii;

    std::string app_name = val["Labels"]["com.docker.compose.project"].asString();
    if (app_name.empty()) {
      continue;
    }

    std::string state = val["State"].asString();
    std::string status = val["Status"].asString();

    Json::Value service_attributes;
    service_attributes["name"] = val["Labels"]["com.docker.compose.service"].asString();
    service_attributes["hash"] = val["Labels"]["io.compose-spec.config-hash"].asString();
    service_attributes["image"] = val["Image"].asString();
    service_attributes["state"] = state;
    service_attributes["status"] = status;

    // (created|restarting|running|removing|paused|exited|dead)
    service_attributes["health"] = "healthy";
    if (status.find("health") != std::string::npos) {
      service_attributes["health"] = getContainerInfo(val["Id"].asString())["State"]["Health"]["Status"].asString();
    } else {
      if (state == "dead" ||
          (state == "exited" && getContainerInfo(val["Id"].asString())["State"]["ExitCode"].asInt() != 0)) {
        service_attributes["health"] = "unhealthy";
      }
    }

    if (service_attributes["health"] != "healthy") {
      service_attributes["logs"] = getContainerLogs(val["Id"].asString(), 5);
    }

    apps[app_name]["services"].append(service_attributes);

    if (ext_func) {
      ext_func(app_name, apps[app_name]);
    }
  }
  return apps;
}

void DockerClient::pruneImages() {
  // curl -G -X POST --unix-socket <sock> "http://localhost/images/prune" --data-urlencode
  // 'filters={"dangling":{"false":true},"label!":{"aktualizr-no-prune":true}}'
  // filters=%7B%22dangling%22%3A%7B%22false%22%3Atrue%7D%2C%22label%21%22%3A%7B%22aktualizr-no-prune%22%3Atrue%7D%7D
  const std::string cmd{
      "http://localhost/images/"
      "prune?filters=%7B%22dangling%22%3A%7B%22false%22%3Atrue%7D%2C%22label%21%22%3A%7B%22aktualizr-no-prune%22%"
      "3Atrue%7D%7D"};
  auto resp = http_client_->post(cmd, Json::nullValue);
  if (!resp.isOk()) {
    throw std::runtime_error("Failed to prune unused images: " + resp.getStatusStr());
  }
}
void DockerClient::pruneContainers() {
  // curl -G -X POST --unix-socket <sock> "http://localhost/containers/prune" --data-urlencode
  // 'filters={"label!":{"aktualizr-no-prune":true}}'
  // filters=%7B%22label%21%22%3A%7B%22aktualizr-no-prune%22%3Atrue%7D%7D
  const std::string cmd{
      "http://localhost/containers/prune?filters=%7B%22label%21%22%3A%7B%22aktualizr-no-prune%22%3Atrue%7D%7D"};
  auto resp = http_client_->post(cmd, Json::nullValue);
  if (!resp.isOk()) {
    throw std::runtime_error("Failed to prune unused containers: " + resp.getStatusStr());
  }
}

void DockerClient::loadImage(const std::string& image_uri, const Json::Value& load_manifest) {
  // The `/images/load` handler expects an array of load manifests in `manifest.json`
  Json::Value lm{Json::arrayValue};
  lm[0] = load_manifest;
  const auto load_manifest_str{Utils::jsonToStr(lm)};
  const auto tarred_manifest{tarString(load_manifest_str, "manifest.json")};
  // curl --unix-socket <sock>  "http://localhost/images/load?quiet=0" --data-binary @tarred_load_manifest -H
  // "Content-Type: application/x-tar"
  LOG_INFO << "Loading image into docker store " << image_uri;
  // TODO: implement support of the "not quiet" request. In this case, the response is streamed as a "chunked" stream,
  // each "chunk" containing image load progress, so the code can output the progress to the logs.
  // The httpclient doesn't support a HTTP response streaming and it will require some effort to implement it.
  // The code that handle the request is located in https://github.com/moby/moby/blob/master/image/tarexport/load.go.
  const std::string cmd{"http://localhost/images/load?quiet=1"};
  auto resp = http_client_->post(cmd, "application/x-tar", tarred_manifest);
  if (!resp.isOk()) {
    throw std::runtime_error("Failed to load image: " + resp.getStatusStr());
  }
  const auto json_resp{resp.getJson()};
  if (json_resp.isMember("stream")) {
    // It prints "Image loaded; refs: <ref1>, <ref2>, ... <refN>"
    LOG_INFO << resp.getJson()["stream"].asString();
  } else {
    // The load handler sends 200 to a caller before all layers are loaded and image refs are set.
    // A presence of the `stream` field in the response assumes that the load was successful, otherwise
    // the exception is thrown with the response payload which contains the load failure reason.
    throw std::runtime_error("Failed to load image: " + Utils::jsonToStr(json_resp));
  }
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

std::string DockerClient::tarString(const std::string& data, const std::string& file_name_in_tar) {
  struct archive* a;
  struct archive_entry* entry;
  size_t archive_size;
  std::vector<uint8_t> tar_data(2 * data.size() + 500 + 512 + 512);

  a = archive_write_new();
  archive_write_set_format_ustar(a);
  int archive_open_status = archive_write_open_memory(a, tar_data.data(), tar_data.size(), &archive_size);
  if (archive_open_status != ARCHIVE_OK) {
    throw std::runtime_error("Failed to create an in-memory TAR archive: " + std::to_string(archive_open_status));
  }

  entry = archive_entry_new();
  archive_entry_set_pathname(entry, file_name_in_tar.c_str());
  archive_entry_set_size(entry, data.size());
  archive_entry_set_filetype(entry, AE_IFREG);
  archive_entry_set_perm(entry, 0644);
  archive_write_header(a, entry);

  archive_write_data(a, data.c_str(), data.size());
  archive_write_finish_entry(a);
  archive_entry_free(entry);

  archive_write_close(a);
  archive_write_free(a);

  return {tar_data.begin(), tar_data.begin() + archive_size};
}

}  // namespace Docker
