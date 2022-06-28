#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <fstream>

#include "test_utils.h"
#include "uptane_generator/image_repo.h"

#include "appengine.h"
#include "liteclient.h"
#include "target.h"

// include test fixtures
#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"
#include "fixtures/liteclient/sysostreerepomock.cc"
#include "fixtures/liteclient/sysrootfs.cc"
#include "fixtures/liteclient/tufrepomock.cc"

class OfflineMetaFetcher : public Uptane::IMetadataFetcher {
 public:
  OfflineMetaFetcher(const boost::filesystem::path& tuf_repo_path) : tuf_repo_path_{tuf_repo_path / "repo" / "repo"} {}

  void fetchRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo, const Uptane::Role& role,
                 Uptane::Version version) const override {
    const boost::filesystem::path meta_file_path{tuf_repo_path_ / version.RoleFileName(role)};
    if (!boost::filesystem::exists(meta_file_path)) {
      throw Uptane::MetadataFetchFailure(repo.ToString(), role.ToString());
    }
    std::ifstream meta_file_stream(meta_file_path.string());
    *result = {std::istreambuf_iterator<char>(meta_file_stream), std::istreambuf_iterator<char>()};
  }

  void fetchLatestRole(std::string* result, int64_t maxsize, Uptane::RepositoryType repo,
                       const Uptane::Role& role) const override {
    fetchRole(result, maxsize, repo, role, Uptane::Version());
  }

 private:
  const boost::filesystem::path tuf_repo_path_;
};

class AkliteOffline : public ::testing::Test {
 protected:
  AkliteOffline()
      : sys_rootfs_{(test_dir_.Path() / "sysroot-fs").string(), branch, hw_id, os},
        sys_repo_{(test_dir_.Path() / "sysrepo").string(), os},
        ostree_repo_{(test_dir_.Path() / "treehub").string(), true},
        tuf_repo_{test_dir_.Path() / "tuf"},
        meta_fetcher_{new OfflineMetaFetcher(tuf_repo_.getPath())} {
    // a path to the config dir
    cfg_.storage.path = test_dir_.Path() / "sota-dir";

    // ostree-based sysroot config
    cfg_.pacman.sysroot = sys_repo_.getPath();
    cfg_.pacman.os = os;
    cfg_.pacman.booted = BootedType::kStaged;
  }

  Uptane::Target addTarget(const std::vector<AppEngine::App>* apps = nullptr) {
    const auto& latest_target{tuf_repo_.getLatest()};
    std::string version;
    if (version.size() == 0) {
      try {
        version = std::to_string(std::stoi(latest_target.custom_version()) + 1);
      } catch (...) {
        LOG_INFO << "No target available, preparing the first version";
        version = "1";
      }
    }

    // update rootfs and commit it into Treehub's repo
    const std::string unique_content = Utils::randomUuid();
    const std::string unique_file = Utils::randomUuid();
    Utils::writeFile(sys_rootfs_.path + "/" + unique_file, unique_content, true);
    auto hash = ostree_repo_.commit(sys_rootfs_.path, "lmp");

    Json::Value apps_json;
    if (apps) {
      for (const auto& app : *apps) {
        apps_json[app.name]["uri"] = app.uri;
      }
    }

    // add new target to TUF repo
    const std::string name = hw_id + "-" + os + "-" + version;
    return tuf_repo_.addTarget(name, hash, hw_id, version, apps_json);
  }

 protected:
  static const std::string branch;
  static const std::string hw_id;
  static const std::string os;

 protected:
  TemporaryDirectory test_dir_;  // must be the first element in the class
  Config cfg_;
  SysRootFS sys_rootfs_;
  SysOSTreeRepoMock sys_repo_;
  OSTreeRepoMock ostree_repo_;
  TufRepoMock tuf_repo_;

  std::shared_ptr<Uptane::IMetadataFetcher> meta_fetcher_;
};

const std::string AkliteOffline::branch{"lmp"};
const std::string AkliteOffline::hw_id{"raspberrypi4-64"};
const std::string AkliteOffline::os{"lmp"};

TEST_F(AkliteOffline, FetchMeta) {
  LiteClient client(cfg_, nullptr, nullptr, meta_fetcher_);
  const auto target{addTarget()};

  ASSERT_TRUE(client.checkForUpdatesBegin());
  const auto targets{client.allTargets()};
  ASSERT_GE(targets.size(), 1);
  ASSERT_EQ(targets[0].filename(), target.filename());
  ASSERT_NO_THROW(client.checkForUpdatesEnd(targets[0]));
}

int main(int argc, char** argv) {
  if (argc != 2) {
    std::cerr << argv[0] << " invalid arguments\n";
    return EXIT_FAILURE;
  }

  ::testing::InitGoogleTest(&argc, argv);
  logger_init();

  SysRootFS::CreateCmd = argv[1];
  return RUN_ALL_TESTS();
}
