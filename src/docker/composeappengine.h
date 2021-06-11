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
  std::string runningApps() const override;

 private:
  static constexpr const char* const MetaDir{".meta"};
  static constexpr const char* const VersionFile{".version"};
  static constexpr const char* const StateFile{".state"};

  class AppState {
   public:
    class File {
     public:
      File(const std::string& path) : path_{path} {}
      ~File() = default;

      void write(int val);
      void write(const std::string& val);

      int read() const;
      std::string readStr() const;

     private:
      static int open(const char* file);
      void write(const void* data, ssize_t size);
      void read(void* data, ssize_t size) const;

      const std::string path_;
      int fd_;
    };

   public:
    enum class State {
      kUnknown = 0,
      kDownloaded = 0x10,
      kDownloadFailed,
      kVerified = 0x20,
      kVerifyFailed,
      kPulled = 0x30,
      kPullFailed,
      kInstalled = 0x40,
      kInstallFail,
      kStarted = 0x50,
      kStartFailed
    };

    AppState(const App& app, const boost::filesystem::path& root, bool set_version = false);
    ~AppState() = default;
    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;

    void setState(const State& state);
    const State& operator()() const { return state_; }

   private:
    std::string version_;
    State state_;

    File version_file_;
    File state_file_;
  };

 private:
  bool cmd_streaming(const std::string& cmd, const App& app);
  static std::pair<bool, std::string> cmd(const std::string& cmd);

  bool download(const App& app);
  bool verify(const App& app);
  bool pullImages(const App& app);
  bool installApp(const App& app);
  bool start(const App& app);

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
