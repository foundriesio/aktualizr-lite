class OSTreeRepoMock {
 public:
  OSTreeRepoMock(std::string path, bool create = false, std::string mode = "archive") : path_{std::move(path)}
  {
    if (!create) return;
    executeCmd("ostree", { "init", "--repo", path_, "--mode=" + mode }, "init an ostree repo at " + path_);
  }

  void pullLocal(const std::string& src_dir, const std::string& hash) {
    executeCmd("ostree", { "pull-local", "--repo", path_, src_dir, hash },
                         "pulling " + hash + " from " + src_dir + " to " + path_);
  }


  std::string commit(const std::string& src_dir, const std::string& branch) {
    return executeCmd("ostree", { "commit", "--repo", path_, "--branch", branch, "--tree=dir=" + src_dir },
                      "commit from " + src_dir + " to " + path_);
  }

  void set_mode(const std::string& mode) {
    executeCmd("ostree", { "config", "--repo", path_, "set", "core.mode", mode }, "set mode for repo " + path_);
  }

  Json::Value generate_delta(const std::string& from, const std::string& to, bool add_stat = false) {
    executeCmd("ostree", {"static-delta", "generate", "--repo", path_, "--from", from, "--to", to},
               "generate static delta between " + from + " and " + to);
    executeCmd("ostree", {"summary", "--repo", path_, "-u"}, "update summary with delta indexes");
    if (add_stat) {
      return get_delta_stat(from, to);
    } else {
      return Json::nullValue;
    }
  }

  Json::Value get_delta_stat(const std::string& from, const std::string& to) {
    const std::string output{executeCmd("ostree", {"static-delta", "show", "--repo", path_, from + "-" + to},
                      "get static delta stats between " + from + " and " + to)};
    std::vector<std::string> output_lines;
    boost::split(output_lines, output, boost::is_any_of("\n"));

    // parse "Total Uncompressed Size: 13832 (13.8 kB)"
    std::vector<std::string> line_elements;
    boost::split(line_elements, output_lines[output_lines.size() - 1], boost::is_any_of(" "));
    boost::trim(line_elements[3]);
    const uint64_t uncomressed_size{boost::lexical_cast<uint64_t>(line_elements[3])};

    // parse "Total Size: 13801 (13.8 kB)"
    boost::split(line_elements, output_lines[output_lines.size() - 2], boost::is_any_of(" "));
    boost::trim(line_elements[2]);
    const uint64_t size{boost::lexical_cast<uint64_t>(line_elements[2])};

    Json::Value stat_json;
    stat_json[to][from]["size"] = size;
    stat_json[to][from]["u_size"] = uncomressed_size;
    const auto stat{Utils::jsonToCanonicalStr(stat_json)};
    const auto hash{boost::algorithm::to_lower_copy(boost::algorithm::hex(Crypto::sha256digest(stat)))};
    Utils::writeFile(path_ + "/delta-stats/" + hash, stat);
    Json::Value delta_stat_ref;
    delta_stat_ref["size"] = stat.size();
    delta_stat_ref["sha256"] = hash;
    return delta_stat_ref;
  }

  const std::string& getPath() const { return path_; }

 private:
  const std::string path_;
};


