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
  static constexpr const char* const ComposeFile{"docker-compose.yml"};

  ComposeAppEngine(boost::filesystem::path root_dir, std::string compose_bin, AppEngine::Client::Ptr client,
                   Docker::RegistryClient::Ptr registry_client);

  Result fetch(const App& app) override;
  Result verify(const App& app) override;
  Result install(const App& app) override;
  Result run(const App& app) override;
  void stop(const App& app) override;
  void remove(const App& app) override;
  bool isFetched(const App& app) const override;
  bool isRunning(const App& app) const override;
  Json::Value getRunningAppsInfo() const override;
  void prune(const Apps& app_shortlist) override {
    (void)app_shortlist;
    pruneDockerStore();
  }

  static void pruneDockerStore();

 protected:
  virtual void pullImages(const App& app);
  virtual void installApp(const App& app);
  virtual void runComposeCmd(const App& app, const std::string& cmd, const std::string& err_msg) const;
  boost::filesystem::path appRoot(const App& app) const { return root_ / app.name; }
  const std::string compose_;

 private:
  static constexpr const char* const MetaDir{".meta"};
  static constexpr const char* const VersionFile{".version"};
  static constexpr const char* const StateFile{".state"};

  class AppState {
   public:
    class File {
     public:
      explicit File(std::string path) : path_{std::move(path)} {}
      ~File() = default;
      File(const File&) = delete;
      File(File&&) = delete;
      File& operator=(const File&) = delete;
      File& operator=(File&&) = delete;

      void write(int val);
      void write(const std::string& val);

      int read() const;
      std::string readStr() const;

     private:
      static int open(const char* file);
      void write(const void* data, ssize_t size);
      void read(void* data, ssize_t size) const;

      const std::string path_;
    };

    enum class State {
      kUnknown = 0,
      kDownloaded = 0x10,
      kVerified = 0x20,
      kPulled = 0x30,
      kInstalled = 0x40,
      kStarted = 0x50
    };

    AppState(const App& app, const boost::filesystem::path& root, bool set_version = false);
    ~AppState() = default;
    AppState(const AppState&) = delete;
    AppState& operator=(const AppState&) = delete;
    AppState(AppState&&) = delete;
    AppState& operator=(AppState&&) = delete;

    static bool exists(const boost::filesystem::path& root);

    void setState(const State& state);
    const State& operator()() const { return state_; }
    const std::string& version() const { return version_; }
    std::string toStr() const {
      static std::map<State, std::string> state2Str = {{State::kUnknown, "Unknown"},
                                                       {State::kDownloaded, "Downloaded"},
                                                       {State::kVerified, "Compose file verified"},
                                                       {State::kPulled, "Images are pulled"},
                                                       {State::kInstalled, "Created"},
                                                       {State::kStarted, "Started"}};

      return state2Str[state_];
    }

   private:
    std::string version_;
    State state_{State::kUnknown};

    File version_file_;
    File state_file_;
  };

  static std::pair<bool, std::string> cmd(const std::string& cmd);

  void download(const App& app);
  void start(const App& app);
  bool areContainersCreated(const App& app);

  static bool checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size);
  void verifyAppArchive(const App& app, const std::string& archive_file_name);
  void extractAppArchive(const App& app, const std::string& archive_file_name, bool delete_after_extraction = true);

  const boost::filesystem::path root_;
  AppEngine::Client::Ptr client_;
  Docker::RegistryClient::Ptr registry_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_COMPOSE_APP_ENGINE_H_
