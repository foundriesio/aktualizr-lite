#include "fixtures/basehttpclient.cc"

namespace fixtures {

class DockerDaemon {
 public:
  // tests/docker-compose_fake.py fills/populates this file with "running" containers
  static constexpr const char* const ContainersFile{"containers.json"};
  static constexpr const char* const ImagesFile{"images.json"};
 public:
  DockerDaemon(boost::filesystem::path dir): dir_{std::move(dir)} {
    // zero containers are pulled and running in the beginning
    Utils::writeFile(dir_ / ContainersFile, std::string("[]"), true);
    Utils::writeFile(dir_ / ImagesFile, std::string("[]"), true);
  }

  std::shared_ptr<HttpInterface> getClient() { return std::make_shared<DockerDaemon::HttpClient>(*this); }
  const boost::filesystem::path& dir() const { return dir_; }
  std::string getRunningContainers() const { return Utils::readFile(dir_ / ContainersFile); }
  std::string getPulledImages() const { return Utils::readFile(dir_ / ImagesFile); }

 private:
  class HttpClient: public BaseHttpClient {
   public:
    HttpClient(DockerDaemon& daemon):daemon_{daemon} {}
    HttpResponse get(const std::string &url, int64_t maxsize) override {
      if (url.find("/images") != std::string::npos) {
        return HttpResponse(daemon_.getPulledImages(), 200, CURLE_OK, "");
      } else if (url.find("/containers") != std::string::npos) {
        return HttpResponse(daemon_.getRunningContainers(), 200, CURLE_OK, "");
      } else {
        return HttpResponse("", 400, CURLE_OK, "Bad Request");
      }
    }
   private:
    DockerDaemon& daemon_;
  };

 private:
  boost::filesystem::path dir_;
};

} // namespace fixtures
