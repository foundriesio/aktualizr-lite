#ifndef AKTUALIZR_LITE_DOCKER_APP_STORE_H_
#define AKTUALIZR_LITE_DOCKER_APP_STORE_H_

#include <string>
#include <memory>

#include <boost/filesystem.hpp>
#include "appengine.h"


namespace Docker {

class AppStore {
 public:
  static constexpr const char* const ArchiveExt{".tgz"};
  using Ptr = std::shared_ptr<AppStore>;
 public:
  virtual boost::filesystem::path appRoot(const AppEngine::App& app) const = 0;
  virtual boost::filesystem::path appArchive(const AppEngine::App& app) const = 0;
  virtual bool pullFromRegistry(const std::string& uri, const std::string& auth = "") const = 0;
  virtual bool copyToDockerStore(const std::string& image) const = 0;

};

class SkopeoAppStore: public AppStore {
 public:
  static constexpr const char* const ManifestFormat{"v2s2"};
  SkopeoAppStore(std::string skopeo_bin, boost::filesystem::path root);

 public:
  boost::filesystem::path appRoot(const AppEngine::App& app) const override { return apps_root_ / app.name; }
  boost::filesystem::path appArchive(const AppEngine::App& app) const override;
  bool pullFromRegistry(const std::string& uri, const std::string& auth = "") const override;
  bool copyToDockerStore(const std::string& image) const override;

 private:
  static bool runCmd(const std::string& cmd);
 private:
  const std::string skopeo_bin_;
  const boost::filesystem::path root_;
  const boost::filesystem::path apps_root_{root_ / "apps"};
  const boost::filesystem::path images_root_{root_ / "images"};
  const boost::filesystem::path images_blobs_root_{root_ / "images" / "blobs"};
};

} // Docker

#endif // AKTUALIZR_LITE_DOCKER_APP_STORE_H_
