class TufRepoMock {
 public:
  TufRepoMock(const boost::filesystem::path& _root, std::string expires = "",
              std::string correlation_id = "corellatio-id")
      : root_{_root.string()}, repo_{_root, expires, correlation_id}, latest_{Uptane::Target::Unknown()}
  {
    repo_.generateRepo(KeyType::kED25519);
  }

 public:
  const std::string& getPath() const { return root_; }
  const Uptane::Target& getLatest() const { return latest_; }

  Uptane::Target addTarget(const std::string& name, const std::string& hash, const std::string& hardware_id,
                           const std::string& version, const Json::Value& apps_json = Json::Value()) {
    Delegation null_delegation{};
    Hash hash_obj{Hash::Type::kSha256, hash};

    Json::Value custom_json;
    custom_json["targetFormat"] = "OSTREE";
    custom_json["version"] = version;
    custom_json["uri"] = "https://ci.foundries.io/projects/factory/lmp/builds/1097";

    custom_json[Target::ComposeAppField] = apps_json;
    repo_.addCustomImage(name, hash_obj, 0, hardware_id, "", 0, null_delegation, custom_json);

    Json::Value target_json;
    target_json["length"] = 0;
    target_json["hashes"]["sha256"] = hash;
    target_json["custom"] = custom_json;
    latest_ = Uptane::Target(name, target_json);
    return latest_;
  }

 private:
  const std::string root_;
  ImageRepo repo_;
  Uptane::Target latest_;
};


