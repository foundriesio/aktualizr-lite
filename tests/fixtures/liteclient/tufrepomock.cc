class TufRepoMock {
 public:
  TufRepoMock(const boost::filesystem::path& _root, std::string expires = "",
              std::string correlation_id = "corellation-id", bool generate_keys = true)
      : root_{_root.string()}, repo_{_root, expires, correlation_id}, latest_{Uptane::Target::Unknown()}
  {
    if (generate_keys) {
      repo_.generateRepo(KeyType::kED25519);
    }
  }

  ~TufRepoMock() { boost::filesystem::remove_all(root_);}

 public:
  const std::string& getPath() const { return root_.string(); }
  std::string getRepoPath() const { return (root_ / ImageRepo::dir).string(); }
  const Uptane::Target& getLatest() const { return latest_; }
  void setLatest(const Uptane::Target& latest) { latest_ = latest; }

  Uptane::Target addTarget(const std::string& name, const std::string& hash, const std::string& hardware_id,
                           const std::string& version, const Json::Value& apps_json = Json::Value(), const Json::Value& delta_stat = Json::Value()) {
    Delegation null_delegation{};
    Hash hash_obj{Hash::Type::kSha256, hash};

    Json::Value custom_json;
    custom_json["targetFormat"] = "OSTREE";
    custom_json["version"] = version;
    custom_json["uri"] = "https://ci.foundries.io/projects/factory/lmp/builds/1097";
    custom_json["hardwareIds"][0] = hardware_id;
    custom_json["ecuIdentifiers"]["test_primary_ecu_serial_id"]["hardwareId"] = hardware_id;
    custom_json["tags"][0] = "default-tag";

    custom_json[Target::ComposeAppField] = apps_json;
    repo_.addCustomImage(name, hash_obj, 0, hardware_id, "", 0, null_delegation, custom_json);
    if (delta_stat) {
      custom_json["delta-stats"] = delta_stat;
    }

    Json::Value target_json;
    target_json["length"] = 0;
    target_json["hashes"]["sha256"] = hash;
    target_json["custom"] = custom_json;
    latest_ = Uptane::Target(name, target_json);
    return latest_;
  }

  ImageRepo& repo() { return repo_; }

  void reset() {
    boost::filesystem::remove_all(root_);
    repo_.generateRepo(KeyType::kED25519);
  }

 private:
  const boost::filesystem::path root_;
  ImageRepo repo_;
  Uptane::Target latest_;
};


