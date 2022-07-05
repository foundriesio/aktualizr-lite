#ifndef AKTUALIZR_LITE_OFFLINE_CLIENT_H_
#define AKTUALIZR_LITE_OFFLINE_CLIENT_H_

#include <boost/filesystem.hpp>
#include <unordered_map>

#include "libaktualizr/config.h"
#include "uptane/fetcher.h"

#include "docker/dockerclient.h"
#include "liteclient.h"

namespace offline {

struct UpdateSrc {
  boost::filesystem::path TufDir;
  boost::filesystem::path OstreeRepoDir;
  boost::filesystem::path AppsDir;
  std::string TargetName;
};

enum class PostInstallAction { Undefined = -1, NeedReboot, NeedDockerRestart };

namespace client {

PostInstallAction install(const Config& cfg_in, const UpdateSrc& src);
void run(const Config& cfg_in, const UpdateSrc& src,
         std::shared_ptr<HttpInterface> docker_client_http_client =
             Docker::DockerClient::DefaultHttpClientFactory("unix:///var/run/docker.sock"));

}  // namespace client
}  // namespace offline

#endif  // AKTUALIZR_LITE_OFFLINE_CLIENT_H_
