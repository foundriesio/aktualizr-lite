class DeviceGatewayMock {
 public:
  static std::string RunCmd;

 public:
  DeviceGatewayMock(const OSTreeRepoMock& ostree, const TufRepoMock& tuf, std::string certDir = "")
      : ostree_{ostree},
        tuf_{tuf},
        port_{TestUtils::getFreePort()},
        url_{certDir.empty() ? "http://localhost:" + port_ : "https://localhost:" + port_},
        req_headers_file_{tuf_.getPath() + "/headers.json"},
        process_{RunCmd,           "--port",         port_, "--ostree", ostree_.getPath(), "--tuf-repo", tuf_.getPath(),
    "--headers-file", req_headers_file_, "--mtls", certDir}
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
  const std::string& getPort() const { return port_; }
  Json::Value getReqHeaders() const { return Utils::parseJSONFile(req_headers_file_); }

 private:
  const OSTreeRepoMock& ostree_;
  const TufRepoMock& tuf_;
  const std::string port_;
  const std::string url_;
  const std::string req_headers_file_;
  boost::process::child process_;
};

std::string DeviceGatewayMock::RunCmd;


