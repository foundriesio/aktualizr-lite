#ifndef AKTUALIZR_LITE_CONTAINERD_CLIENT_H
#define AKTUALIZR_LITE_CONTAINERD_CLIENT_H

#include "appengine.h"

namespace containerd {

class Client : public AppEngine::Client {
 public:
  explicit Client(std::string nerdctl_path);

  void getContainers(Json::Value& root) override;
  std::tuple<bool, std::string> getContainerState(const Json::Value& root, const std::string& app,
                                                  const std::string& service, const std::string& hash) const override;
  std::string getContainerLogs(const std::string& id, int tail) override;
  const Json::Value& engineInfo() const override;
  const std::string& arch() const override;
  Json::Value getRunningApps(const std::function<void(const std::string&, Json::Value&)>& ext_func) override;

 private:
  const std::string nerdctl_;
};

}  // namespace containerd

#endif  // AKTUALIZR_LITE_CONTAINERD_CLIENT_H
