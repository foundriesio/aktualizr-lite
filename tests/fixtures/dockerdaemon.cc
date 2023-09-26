#include "fixtures/basehttpclient.cc"

#include "libaktualizr/http/httpclient.h"

namespace fixtures {

class DockerDaemon {
 public:
  static std::string RunCmd;
 public:
  // tests/docker-compose_fake.py fills/populates this file with "running" containers
  static constexpr const char* const ContainersFile{"containers.json"};
  static constexpr const char* const ImagePullFailFlag{"image-pull-fails"};
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

  void setImagePullFailFlag(bool fail) {
    if (fail) {
      Utils::writeFile(dir_ / ImagePullFailFlag, std::string(""));
    } else {
      boost::filesystem::remove(dir_ / ImagePullFailFlag);
    }
  }

  bool isImagePullFailSet() const {
    return boost::filesystem::exists(dir_ / ImagePullFailFlag);
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

    HttpResponse post(const std::string& url, const std::string& content_type, const std::string& data) override {
      if (std::string::npos != url.rfind("/images/load") && content_type == "application/x-tar") {
        if (daemon_.isImagePullFailSet()) {
          return HttpResponse("", 500, CURLE_OK, "");
        }
        // We need to make to the `docker-compose_fake` think that the image is installed/pulled/loaded.
        // To do so, we have to set `docker-daemon-dir/images.json::[image URI]` to true.
        // The image URI is not available at the http post request neither in headers nor in query params,
        // it's embedded in the TAR archive, in `RepoTags` field of the TARred `manifest.json`.
        // Let the daemon mock written in Python do the TAR extraction and updating of the `images.json`.
        ::HttpClient c{daemon_.unix_sock_.PathString()};
        return c.post(url, content_type, data);
      }
      return BaseHttpClient::post(url, content_type, data);
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
