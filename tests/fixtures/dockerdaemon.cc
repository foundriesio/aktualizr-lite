#include "fixtures/basehttpclient.cc"

namespace fixtures {

class DockerDaemon {
 public:
  static std::string RunCmd;
 public:
  // tests/docker-compose_fake.py fills/populates this file with "running" containers
  static constexpr const char* const ContainersFile{"containers.json"};
 public:
  DockerDaemon(boost::filesystem::path dir): dir_{std::move(dir)}, port_{TestUtils::getFreePort()}, process_{RunCmd, "--port", port_, "--dir", dir.string()} {
    // zero containers are running in the beginning
    Utils::writeFile(dir_ / ContainersFile, std::string("[]"), true);
    TestUtils::waitForServer("http://localhost:" + port_ + "/");
  }

  std::string getUrl() const {
    return "http://localhost:" + port_;
  }

  std::shared_ptr<HttpInterface> getClient() { return std::make_shared<DockerDaemon::HttpClient>(*this); }
  const boost::filesystem::path& dir() const { return dir_; }
  std::string getRunningContainers() const { return Utils::readFile(dir_ / ContainersFile); }

 private:
  class HttpClient: public BaseHttpClient {
   public:
    HttpClient(DockerDaemon& daemon):daemon_{daemon} {}
    HttpResponse get(const std::string &url, int64_t maxsize) override {
      if (std::string::npos != url.rfind("version")) {
        Json::Value info;
        info["Arch"] = "amd64";
        const auto resp_str{Utils::jsonToStr(info)};

        return HttpResponse(resp_str, 200, CURLE_OK, "");
      }

      return HttpResponse(daemon_.getRunningContainers(), 200, CURLE_OK, "");
    }
   private:
    DockerDaemon& daemon_;
  };

 private:
  boost::filesystem::path dir_;
  const std::string port_;
  boost::process::child process_;
};

std::string DockerDaemon::RunCmd{"./tests/docker-daemon_fake.py"};

} // namespace fixtures
