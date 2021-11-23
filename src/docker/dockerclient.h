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
  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>()>;
  static HttpClientFactory DefaultHttpClientFactory;

  explicit DockerClient(std::shared_ptr<HttpInterface> http_client = DefaultHttpClientFactory());
  void getContainers(Json::Value& root);

  static std::tuple<bool, std::string> getContainerState(const Json::Value& root, const std::string& app,
                                                         const std::string& service, const std::string& hash);

 private:
  std::shared_ptr<HttpInterface> http_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_CLIENT_H
