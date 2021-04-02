#ifndef AKTUALIZR_LITE_COMPOSE_APP_ENGINE_H_
#define AKTUALIZR_LITE_COMPOSE_APP_ENGINE_H_

#include <string>

#include <boost/filesystem.hpp>

#include "appengine.h"
#include "docker/docker.h"
#include "docker/dockerclient.h"

namespace Docker {

class ComposeAppEngine : public AppEngine {
 public:
  static constexpr const char* const ArchiveExt{".tgz"};
  static constexpr const char* const NeedStartFile{".need_start"};
  static constexpr const char* const ComposeFile{"docker-compose.yml"};

 public:
  ComposeAppEngine(boost::filesystem::path root_dir, std::string compose_bin, Docker::DockerClient::Ptr docker_client,
                   Docker::RegistryClient::Ptr registry_client);

  bool fetch(const App& app) override;
  bool install(const App& app) override;
  bool run(const App& app) override;
  void remove(const App& app) override;
  bool isRunning(const App& app) const override;

 private:
  bool cmd_streaming(const std::string& cmd, const App& app);
  static std::pair<bool, std::string> cmd(const std::string& cmd);
  bool download(const App& app);
  static bool checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size);
  void extractAppArchive(const App& app, const std::string& archive_file_name, bool delete_after_extraction = true);
  boost::filesystem::path appRoot(const App& app) const { return root_ / app.name; }

 private:
  const boost::filesystem::path root_;
  const std::string compose_;
  Docker::DockerClient::Ptr docker_client_;
  Docker::RegistryClient::Ptr registry_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_COMPOSE_APP_ENGINE_H_
