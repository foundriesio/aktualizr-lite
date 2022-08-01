#ifndef AKTUALIZR_LITE_DOCKER_CLIENT_H
#define AKTUALIZR_LITE_DOCKER_CLIENT_H
#include <json/json.h>
#include <functional>
#include <string>

#include "http/httpinterface.h"

namespace Docker {

class DockerClient {
 public:
  using Ptr = std::shared_ptr<DockerClient>;
  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>(const std::string& docker_host)>;
  static const HttpClientFactory DefaultHttpClientFactory;

  explicit DockerClient(
      std::shared_ptr<HttpInterface> http_client = DefaultHttpClientFactory("unix:///var/run/docker.sock"));
  void getContainers(Json::Value& root);

  static std::tuple<bool, std::string> getContainerState(const Json::Value& root, const std::string& app,
                                                         const std::string& service, const std::string& hash);
  std::string getContainerLogs(const std::string& id, int tail = 5);
  const Json::Value& engineInfo() const { return engine_info_; }
  const std::string& arch() const { return arch_; }
  Json::Value getRunningApps(const std::function<void(const std::string&, Json::Value&)>& ext_func = nullptr);

 private:
  Json::Value getEngineInfo();
  Json::Value getContainerInfo(const std::string& id);

  std::shared_ptr<HttpInterface> http_client_;
  const Json::Value engine_info_;
  const std::string arch_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_CLIENT_H
