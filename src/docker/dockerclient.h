#ifndef AKTUALIZR_LITE_DOCKER_CLIENT_H
#define AKTUALIZR_LITE_DOCKER_CLIENT_H

#include <json/json.h>
#include <functional>
#include <string>

#include "http/httpinterface.h"

namespace Docker {

class DockerClient {
 public:
  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>()>;
  static HttpClientFactory DefaultHttpClientFactory;

 public:
  DockerClient(const std::string& app, bool curl = false,
               const HttpClientFactory& http_client_factory = DefaultHttpClientFactory);
  bool serviceRunning(std::string& service, std::string& hash);

 private:
  Json::Value root_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_CLIENT_H
