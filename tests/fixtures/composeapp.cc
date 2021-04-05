namespace fixtures {

class ComposeApp {
 public:
  using Ptr = std::shared_ptr<ComposeApp>;
  const std::string DefaultTemplate = R"(
    services:
      %s:
        image: %s
        labels:
          io.compose-spec.config-hash: %s
    version: "3.2"
    )";

  const std::string ServiceTemplate = R"(
    %s:
      image: %s
    )";

 public:
  static Ptr create(const std::string& name,
                    const std::string& service = "service-01", const std::string& image = "image-01") {
    Ptr app{new ComposeApp(name)};
    app->updateService(service, image);
    return app;
  }

  const std::string& updateService(const std::string& service, const std::string& image) {
    char service_content[1024];
    sprintf(service_content, ServiceTemplate.c_str(), service.c_str(), image.c_str());
    auto service_hash = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(service_content)));
    sprintf(content_, DefaultTemplate.c_str(), service.c_str(), image.c_str(), service_hash.c_str());
    return update();
  }

  const std::string& name() const { return name_; }
  const std::string& hash() const { return hash_; }
  const std::string& archHash() const { return arch_hash_; }
  const std::string& archive() const { return arch_; }
  const std::string& manifest() const { return manifest_; }


 private:
  ComposeApp(const std::string& name):name_{name} {}

  const std::string& update() {
    TemporaryDirectory app_dir;
    TemporaryFile arch_file{"arch.tgz"};

    Utils::writeFile(app_dir.Path() / Docker::ComposeAppEngine::ComposeFile, std::string(content_));
    boost::process::system("tar -czf " + arch_file.Path().string() + " .",  boost::process::start_dir = app_dir.Path());
    arch_ = Utils::readFile(arch_file.Path());
    arch_hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(arch_)));

    Json::Value manifest;
    manifest["annotations"]["compose-app"] = "v1";
    manifest["layers"][0]["digest"] = "sha256:" + arch_hash_;
    manifest["layers"][0]["size"] = arch_.size();
    manifest_ = Utils::jsonToCanonicalStr(manifest);
    hash_ = boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(manifest_)));
    return hash_;
  }

 private:
  const std::string name_;
  char content_[4096];

  std::string arch_;
  std::string arch_hash_;
  std::string manifest_;
  std::string hash_;
};


} // namespace fixtures
