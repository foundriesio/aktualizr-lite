#include "fixtures/basehttpclient.cc"

namespace fixtures {

class DockerDaemon {
 public:
  static std::string RunCmd;
 public:
  // tests/docker-compose_fake.py fills/populates this file with "running" containers
  static constexpr const char* const ContainersFile{"containers.json"};
 public:
  // run two processes, one (http) is needed for the test business logic, another one (unix) to mock docker daemon
  // since there are spots in the code where the `docker` CLI is invoked directly.
  DockerDaemon(boost::filesystem::path dir): dir_{std::move(dir)}, port_{TestUtils::getFreePort()},
               unix_process_{RunCmd, "-u", unix_sock_.PathString(), "--dir", dir_.string()},
               process_{RunCmd, "--port", port_, "--dir", dir_.string()} {
    // zero containers are running in the beginning
    Utils::writeFile(dir_ / ContainersFile, none_containers_, true);
    TestUtils::waitForServer("http://localhost:" + port_ + "/");
  }

  ~DockerDaemon() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
    unix_process_.terminate();
    unix_process_.wait_for(std::chrono::seconds(10));
  }

  const std::string& dataRoot() const { return dir_.string(); }

  std::string getUrl() const { return "http://localhost:" + port_; }
  std::string getUnixSocket() const { return "unix://" +  unix_sock_.PathString(); }

  std::shared_ptr<HttpInterface> getClient() { return std::make_shared<DockerDaemon::HttpClient>(*this); }
  const boost::filesystem::path& dir() const { return dir_; }
  std::string getRunningContainers() const { return Utils::readFile(dir_ / ContainersFile); }
  bool areContainersCreated() const {
    const std::string cur_containers{Utils::readFile(dir_ / ContainersFile)};
    return !(cur_containers == none_containers_);
  }


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
  TemporaryFile unix_sock_;
  const std::string none_containers_{"[]"};
  boost::filesystem::path dir_;
  const std::string port_;
  boost::process::child unix_process_;
  boost::process::child process_;
};

std::string DockerDaemon::RunCmd{"./tests/docker-daemon_fake.py"};

} // namespace fixtures
