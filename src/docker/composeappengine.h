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
  static constexpr const char* const ComposeFile{"docker-compose.yml"};

 public:
  ComposeAppEngine(boost::filesystem::path root_dir, std::string compose_bin, Docker::DockerClient::Ptr docker_client,
                   Docker::RegistryClient::Ptr registry_client);

  bool fetch(const App& app) override;
  bool install(const App& app) override;
  bool run(const App& app) override;
  void remove(const App& app) override;
  bool isRunning(const App& app) const override;
  Json::Value getRunningAppsInfo() const override;
  void purge(const Apps& app_shortlist) const override {}

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

    static bool exists(const boost::filesystem::path& root);

    void setState(const State& state);
    const State& operator()() const { return state_; }
    const std::string& version() const { return version_; }
    std::string toStr() const {
      static std::map<State, std::string> state2Str = {
          {State::kUnknown, "Unknown"},
          {State::kDownloaded, "Downloaded"},
          {State::kDownloadFailed, "Download failed"},
          {State::kVerified, "Compose file verified"},
          {State::kVerifyFailed, "Compose file verification failure"},
          {State::kPulled, "Images are pulled"},
          {State::kPullFailed, "Image pull failed"},
          {State::kInstalled, "Created"},
          {State::kInstallFail, "Creation failed"},
          {State::kStarted, "Started"},
          {State::kStartFailed, "Start failed"},
      };

      return state2Str[state_];
    }

   private:
    std::string version_{""};
    State state_{State::kUnknown};

    File version_file_;
    File state_file_;
  };

 public:
  virtual boost::filesystem::path appRoot(const App& app) const { return root_ / app.name; }

 protected:
  virtual bool download(const App& app);
  virtual bool verify(const App& app);
  virtual bool pullImages(const App& app);
  virtual bool installApp(const App& app);
  virtual bool start(const App& app);
  void extractAppArchive(const App& app, const std::string& archive_file_name, bool delete_after_extraction = false);

  Docker::RegistryClient::Ptr registryClient() { return registry_client_; }

 private:
  bool cmd_streaming(const std::string& cmd, const App& app) const;
  static std::pair<bool, std::string> cmd(const std::string& cmd);

  static bool checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size);
  void verifyAppArchive(const App& app, const std::string& archive_file_name);

 private:
  const boost::filesystem::path root_;
  const std::string compose_;
  Docker::DockerClient::Ptr docker_client_;
  Docker::RegistryClient::Ptr registry_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_COMPOSE_APP_ENGINE_H_
