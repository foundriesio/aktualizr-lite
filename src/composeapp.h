#ifndef AKTUALIZR_LITE_COMPOSEAPP_H_
#define AKTUALIZR_LITE_COMPOSEAPP_H_

#include <string>

#include <boost/filesystem.hpp>

#include "docker.h"

namespace Docker {

class ComposeApp {
 public:
  static constexpr const char* const ArchiveExt{".tgz"};
  static constexpr const char* const NeedStartFile{".need_start"};
  static constexpr const char* const ComposeFile{"docker-compose.yml"};

 public:
  ComposeApp(std::string name, const boost::filesystem::path& root_dir, const std::string& compose_bin,
             const Docker::RegistryClient& registry_client);

  bool fetch(const std::string& app_uri);
  bool up(bool no_start = false);
  bool start();
  void remove();
  bool isRunning() const;

 private:
  bool cmd_streaming(const std::string& cmd);
  std::pair<bool, std::string> cmd(const std::string& cmd) const;
  bool download(const std::string& app_uri);
  static bool checkAvailableStorageSpace(const boost::filesystem::path& app_root, uint64_t& out_available_size);
  void extractAppArchive(const std::string& archive_file_name, bool delete_after_extraction = true);

 private:
  const std::string name_;
  const boost::filesystem::path root_;
  const std::string compose_;
  const Docker::RegistryClient& registry_client_;
};

}  // namespace Docker

#endif  // AKTUALIZR_LITE_COMPOSEAPP_H_
