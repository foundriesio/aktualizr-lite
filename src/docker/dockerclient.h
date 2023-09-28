#ifndef AKTUALIZR_LITE_DOCKER_CLIENT_H
#define AKTUALIZR_LITE_DOCKER_CLIENT_H
#include <json/json.h>
#include <functional>
#include <string>

#include "appengine.h"
#include "http/httpinterface.h"

namespace Docker {

class DockerClient : public AppEngine::Client {
 public:
  using Ptr = std::shared_ptr<DockerClient>;
  using HttpClientFactory = std::function<std::shared_ptr<HttpInterface>(const std::string& docker_host)>;
  static const HttpClientFactory DefaultHttpClientFactory;

  explicit DockerClient(
      std::shared_ptr<HttpInterface> http_client = DefaultHttpClientFactory("unix:///var/run/docker.sock"));

  void getContainers(Json::Value& root) override;
  std::tuple<bool, std::string> getContainerState(const Json::Value& root, const std::string& app,
                                                  const std::string& service, const std::string& hash) const override;
  std::string getContainerLogs(const std::string& id, int tail) override;
  const Json::Value& engineInfo() const override { return engine_info_; }
  const std::string& arch() const override { return arch_; }
  Json::Value getRunningApps(const std::function<void(const std::string&, Json::Value&)>& ext_func) override;
  void pruneImages() override;
  void pruneContainers() override;
  void loadImage(const std::string& image_uri, const Json::Value& load_manifest) override;
  static std::string tarString(const std::string& data, const std::string& file_name_in_tar);

 private:
  Json::Value getEngineInfo();
  Json::Value getContainerInfo(const std::string& id);

  std::shared_ptr<HttpInterface> http_client_;
  const Json::Value engine_info_;
  const std::string arch_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_DOCKER_CLIENT_H
