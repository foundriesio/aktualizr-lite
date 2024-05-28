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
                           const std::string& version, const Json::Value& apps_json = Json::Value(),
                           const Json::Value& delta_stat = Json::Value(),
                           const std::string* ci_app_shortlist = nullptr,
                           const std::string& ci_app_uri = "http://apps.tar") {
    Delegation null_delegation{};
    Hash hash_obj{Hash::Type::kSha256, hash};

    Json::Value custom_json;
    custom_json["targetFormat"] = "OSTREE";
    custom_json["version"] = version;
    custom_json["uri"] = "https://ci.foundries.io/projects/factory/lmp/builds/1097";
    custom_json["hardwareIds"][0] = hardware_id;
    custom_json["ecuIdentifiers"]["test_primary_ecu_serial_id"]["hardwareId"] = hardware_id;
    custom_json["tags"][0] = "default-tag";
    if (ci_app_shortlist != nullptr ) {
      if (ci_app_uri.empty()) {
        custom_json["fetched-apps"]["uri"] = Json::nullValue;
      } else {
        custom_json["fetched-apps"]["uri"] = ci_app_uri;
      }

      custom_json["fetched-apps"]["shortlist"] = *ci_app_shortlist;
    }

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

  KeyPair getTargetsKey() const {
    return repo_.getKey(Uptane::Role::Targets());
  }

  void updateBundleMeta(const std::string& target_name) {
    Json::Value targets_meta{Utils::parseJSONFile(getRepoPath() + "/targets.json")};

    boost::filesystem::path bundle_meta_path{getRepoPath() + "/bundle-targets.json"};
    Json::Value bundle_meta;
    if (boost::filesystem::exists(bundle_meta_path)) {
      bundle_meta = Utils::parseJSONFile(bundle_meta_path);
      bundle_meta["signed"]["x-fio-offline-bundle"]["targets"].append(target_name);
    } else {
      bundle_meta["signed"]["_type"] = "Targets";
      bundle_meta["signed"]["expires"] = targets_meta["signed"]["expires"];
      bundle_meta["signed"]["version"] = targets_meta["signed"]["version"];
      bundle_meta["signed"]["x-fio-offline-bundle"]["targets"][0] = target_name;
      bundle_meta["signed"]["x-fio-offline-bundle"]["type"] = "ci";
      bundle_meta["signed"]["x-fio-offline-bundle"]["tag"] = "default-tag";
    }

    const auto key{getTargetsKey()};

    std::string b64sig = Utils::toBase64(Crypto::Sign(key.public_key.Type(), nullptr, key.private_key,
                                                      Utils::jsonToCanonicalStr(bundle_meta["signed"])));
    Json::Value signature;
    switch (key.public_key.Type()) {
      case KeyType::kRSA2048:
      case KeyType::kRSA3072:
      case KeyType::kRSA4096:
        signature["method"] = "rsassa-pss";
        break;
      case KeyType::kED25519:
        signature["method"] = "ed25519";
        break;
      default:
        throw std::runtime_error("Unknown key type");
    }
    signature["sig"] = b64sig;
    signature["keyid"] = key.public_key.KeyId();
    bundle_meta["signatures"][0] = signature;

    Utils::writeFile(bundle_meta_path, bundle_meta);
  }

  std::string getBundleMetaPath() const {
    return getRepoPath() + "/bundle-targets.json";
  }

 private:
  const boost::filesystem::path root_;
  ImageRepo repo_;
  Uptane::Target latest_;
};


