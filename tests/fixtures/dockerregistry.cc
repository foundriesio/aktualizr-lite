namespace fixtures {

class DockerRegistry {
 public:
  static std::string RunCmd;

 public:
  DockerRegistry(const boost::filesystem::path& dir,
                 const std::string& base_url = "hub.foundries.io",
                 const std::string& auth_url = "https://ota-lite.foundries.io:8443/hub-creds/",
                 const std::string& repo = "factory"):
                 dir_{dir}, base_url_{base_url}, auth_url_{auth_url}, repo_{repo}, port_{TestUtils::getFreePort()}, process_{RunCmd, "--port", port_, "--dir", dir.string()} {
    TestUtils::waitForServer("http://localhost:" + port_ + "/v2/");
  }

  ~DockerRegistry() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

  std::string getSkopeoClient() const {
    const std::string registry_config{"[[registry]]\nprefix = \"localhost\"\ninsecure = true\nlocation = \"localhost:" + port_ + "\""};
    static const std::string registry_config_file{"registries.conf"};

    Utils::writeFile(dir_ / registry_config_file, registry_config);

    return "skopeo --registries-conf " + (dir_ / registry_config_file).string();
  }

  std::shared_ptr<HttpInterface> getClient() { return std::make_shared<HttpClient>(*this); }
  Docker::RegistryClient::HttpClientFactory getClientFactory() {
    return [this](const std::vector<std::string>*) {
      return getClient();
    };
  }

  AppEngine::App addApp(const ComposeApp::Ptr& app) {
    hash2manifest_.emplace("sha256:" + app->layersHash(), app->layersManifest());
    manifest2pull_numb_.emplace("sha256:" + app->layersHash(), 0);

    auto app_uri = base_url_ + '/' + repo_ + '/' + app->name() + '@' + "sha256:" + app->hash();
    hash2manifest_.emplace("sha256:" + app->hash(), app->manifest());
    manifest2pull_numb_.emplace("sha256:" + app->hash(), 0);
    blob2app_.emplace("sha256:" + app->archHash(), app);

    Utils::writeFile(dir_ / app->image().name() / "blobs" / app->image().layerBlob().hash, app->image().layerBlob().data);
    Utils::writeFile(dir_ / app->image().name() / "blobs" / app->image().config().hash, app->image().config().data);
    Utils::writeFile(dir_ / app->image().name() / "manifests" / app->image().manifest().hash, app->image().manifest().data);
    return {app->name(), app_uri};
  }

  std::string getAppManifest(const std::string &url) {
    auto digest = parseUrl(url, "manifests");
    if (hash2manifest_.count(digest) == 0) {
      return ""; //TODO: throw exception
    }
    ++manifest2pull_numb_.at(digest);
    return hash2manifest_.at(digest);
  }

  int getAppManifestPullNumb(const std::string &app_uri) const {
    const Docker::Uri uri{Docker::Uri::parseUri(app_uri)};
    if (hash2manifest_.count(uri.digest()) == 0) {
      return 0;
    }
    return manifest2pull_numb_.at(uri.digest());
  }

  std::string getAppArchive(const std::string &url) const {
    auto digest = parseUrl(url, "blobs");
    if (blob2app_.count(digest) == 0) {
      return ""; //TODO: throw exception
    }
    return blob2app_.at(digest)->archive();
  }

  const std::string& authURL() const { return auth_url_; }

 private:
  class HttpClient: public BaseHttpClient {
   public:
    HttpClient(DockerRegistry& registry):registry_{registry} {}
    HttpResponse get(const std::string &url, int64_t maxsize) override {
      std::string resp;
      if (std::string::npos != url.find(registry_.base_url_ + "/token-auth/")) {
        // request for OAuth token
        resp = "{\"token\":\"token\"}";
      } else if (std::string::npos != url.find(registry_.base_url_ + "/v2/")) {
        // request for manifest
        resp = registry_.getAppManifest(url);
        if (resp.size() == 0) {
          // manifest hasn't been found
          return HttpResponse(resp, 404, CURLE_OK, "Not Found");
        }
      } else if (url == registry_.auth_url_) {
        // request for a basic auth to Device Gateway
        resp = "{\"Secret\":\"secret\",\"Username\":\"test-user\"}";
      } else {
        return HttpResponse(resp, 401, CURLE_OK, "");
      }
      return HttpResponse(resp, 200, CURLE_OK, "");
    }

    HttpResponse download(const std::string &url, curl_write_callback write_cb, curl_xferinfo_callback progress_cb, void *userp, curl_off_t from) override {
      (void)url;
      (void)progress_cb;
      (void)from;

      std::string data{registry_.getAppArchive(url)};
      write_cb(const_cast<char*>(data.c_str()), data.size(), 1, userp);

      return HttpResponse("resp", 200, CURLE_OK, "");
    }
   private:
    DockerRegistry& registry_;
  };

  std::string parseUrl(const std::string& url, const std::string endpoint) const {
    // https://hub.foundries.io/v2/factory/app-01/manifests/sha256:4567bac35dd2a7446448052e0b5745c49a8983b2
    // https://hub.foundries.io/v2/factory/app-01/blobs/sha256:e723bc71a139ad7e1f6cbc117178c74b821a65afec5b

    auto found_pos = url.find(base_url_);
    if (found_pos == std::string::npos) {
      throw std::invalid_argument("Invalid App URL: " + url);
    }

    std::array<std::string, 5> url_elements;

    auto prev_pos = found_pos;
    auto prev_elem_len = base_url_.size();
    for (auto& elem : url_elements) {
      auto elem_pos = prev_pos + prev_elem_len + 1;
      elem = url.substr(elem_pos, url.find('/', elem_pos) - elem_pos);
      prev_pos = elem_pos;
      prev_elem_len = elem.size();
    }

    if (url_elements[0] != "v2") {
      throw std::invalid_argument("Invalid App URL: " + url + "; expected `v2` got " + url_elements[0]);
    }

    if (url_elements[3] != endpoint) {
      throw std::invalid_argument("Invalid App URL: " + url + "; expected `" + endpoint + "` endpoint, got " + url_elements[3]);
    }

    if (url_elements[1] != repo_) {
      throw std::invalid_argument("Invalid App URL: " + url + "; expected `" + repo_ + "`, got " + url_elements[1]);
    }

    return url_elements[4];
  }

  using AppID = std::pair<std::string, std::string>;

 private:
  const boost::filesystem::path dir_;
  const std::string base_url_;
  const std::string auth_url_; // URL to Device Gateway, docker gets initial auth token from it
  const std::string repo_; // repo, aka Factory name

  const std::string port_;
  boost::process::child process_;

  std::unordered_map<std::string, std::string> hash2manifest_;
  std::unordered_map<std::string, int> manifest2pull_numb_;
  std::unordered_map<std::string, ComposeApp::Ptr> blob2app_;
};

std::string DockerRegistry::RunCmd{"./tests/docker-registry_fake.py"};

} // namespace fixtures
