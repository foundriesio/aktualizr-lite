class DeviceGatewayMock {
 public:
  static std::string RunCmd;

 public:
 static std::vector<std::string> getDeviceGatewayArgs(std::vector<std::string>&& args, const std::string& certDir) {
    std::vector<std::string> args_out{args};
    if (!certDir.empty()) {
      args_out.emplace_back("--mtls");
      args_out.emplace_back(certDir);
    }
    return args_out;
 }
 public:
  DeviceGatewayMock(const OSTreeRepoMock& ostree, const TufRepoMock& tuf, std::string certDir = "")
      : ostree_{ostree},
        tuf_{tuf},
        port_{TestUtils::getFreePort()},
        url_{certDir.empty() ? "http://localhost:" + port_ : "https://localhost:" + port_},
        req_headers_file_{tuf_.getPath() + "/headers.json"},
        events_file_{tuf_.getPath() + "/events.json"},
        sota_toml_file_{tuf_.getPath() + "/sota.toml"},
        process_{
          getDeviceGatewayArgs({RunCmd,
                                "--port", port_,
                                "--ostree", ostree_.getPath(),
                                "--tuf-repo", tuf_.getPath(),
                                "--headers-file", req_headers_file_,
                                "--events-file", events_file_,
                                "--sota-toml", sota_toml_file_}
                                , certDir)}
  {
    if (certDir.empty()) {
      TestUtils::waitForServer(url_ + "/");
    } else {
      sleep(1);
    }
    LOG_INFO << "Device Gateway is running on port " << port_;
  }

  ~DeviceGatewayMock() {
    process_.terminate();
    process_.wait_for(std::chrono::seconds(10));
  }

 public:
  std::string getTreeUri() const { return url_ + "/"; }
  std::string getOsTreeUri() const { return url_ + "/treehub"; }
  std::string getTufRepoUri() const { return url_ + "/repo"; }
  std::string getTlsUri() const { return url_; }
  const std::string& getPort() const { return port_; }
  Json::Value getReqHeaders() const { return Utils::parseJSONFile(req_headers_file_); }
  Json::Value getEvents() const { return Utils::parseJSONFile(events_file_); }
  bool resetEvents() const { return boost::filesystem::remove(events_file_); }
  std::string readSotaToml() const { return Utils::readFile(sota_toml_file_); }
  bool resetSotaToml() const { return boost::filesystem::remove(sota_toml_file_); }

 private:
  const OSTreeRepoMock& ostree_;
  const TufRepoMock& tuf_;
  const std::string port_;
  const std::string url_;
  const std::string req_headers_file_;
  const std::string events_file_;
  const std::string sota_toml_file_;
  boost::process::child process_;
};

std::string DeviceGatewayMock::RunCmd;


