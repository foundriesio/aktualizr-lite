#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string/trim.hpp>

#include "logging/logging.h"
#include "utilities/utils.h"
#include "ostree/repo.h"

#include "test_utils.h"
#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"


class OSTreeTest : public ::testing::Test {
 protected:
  OSTreeTest(){
  }

  virtual void SetUp() {
    path_ = mkdtemp(const_cast<char*>((testing::TempDir() + "OSTreeTest-repo-XXXXXX").c_str()));
    repo_ = std::make_unique<OSTree::Repo>(path_, true);
  }

  virtual void TearDown() {
    boost::filesystem::remove_all(path_);
  }

  bool isRepoInited() const {
    return boost::filesystem::exists(path_ + "/config") && boost::filesystem::exists(path_ + "/objects");
  }

 protected:
  std::string path_;
  std::unique_ptr<OSTree::Repo> repo_;
};


TEST_F(OSTreeTest, CreateDestroy) {
  ASSERT_TRUE(isRepoInited());
}

TEST_F(OSTreeTest, InitExistingDestroy) {
  ASSERT_TRUE(isRepoInited());
  OSTree::Repo repo_from_filesystem_no_create(path_);
  OSTree::Repo repo_from_filesystem_create(path_, true);
}

TEST_F(OSTreeTest, InitNonExisting) {
  TemporaryDirectory non_init_repo_dir;
  ASSERT_THROW(OSTree::Repo repo_from_filesystem(non_init_repo_dir.Path().string()), std::runtime_error);
}

TEST_F(OSTreeTest, AddRemote) {
  ASSERT_TRUE(isRepoInited());
  repo_->addRemote("treehub", "http://localhost", "", "", "");
}

TEST_F(OSTreeTest, Config) {
  ASSERT_TRUE(isRepoInited());
  {
    repo_->setConfigItem("core", "min-free-size-required", "1024");
    ASSERT_EQ(repo_->getConfigItem("core", "min-free-size-required"), "1024");
    repo_->unsetConfigItem("core", "min-free-size-required");
    ASSERT_EQ(repo_->getConfigItem("core", "min-free-size-required"), "");
  }
  {
    ASSERT_EQ(repo_->getConfigItem("foo", "bar"), "");
    ASSERT_NO_THROW(repo_->unsetConfigItem("foo1", "bar"));
  }
}

TEST_F(OSTreeTest, PullLocal) {
  ASSERT_TRUE(isRepoInited());

  TemporaryDirectory test_dir;
  boost::filesystem::path src_repo_path{test_dir.Path() / "repo"};
  boost::filesystem::path src_repo_fs{test_dir.Path() / "repo_fs"};
  boost::filesystem::path test_file_01{"foo.bar"};
  const std::string test_file_content_01{"some dummy content"};
  boost::filesystem::path test_file_02{"file02.txt"};
  const std::string test_file_content_02{"another dummy content"};
  boost::filesystem::path dst_repo_fs{test_dir.Path() / "dst_fs"};
  OSTreeRepoMock src_repo{src_repo_path.string(), true};



  Utils::writeFile(src_repo_fs / test_file_01, test_file_content_01);
  Utils::writeFile(src_repo_fs / test_file_02, test_file_content_02);

  const auto commit_hash{src_repo.commit(src_repo_fs.string(), "lmp")};
  ASSERT_NO_THROW(repo_->pullLocal(src_repo.getPath(), commit_hash));
  ASSERT_NO_THROW(repo_->checkout(commit_hash, "/", dst_repo_fs.string()));
  ASSERT_TRUE(boost::filesystem::exists( dst_repo_fs / test_file_01));
  ASSERT_TRUE(boost::filesystem::exists( dst_repo_fs / test_file_02));
  ASSERT_EQ(Utils::readFile(dst_repo_fs / test_file_01), test_file_content_01);
  ASSERT_EQ(Utils::readFile(dst_repo_fs / test_file_02), test_file_content_02);
}

// TODO: Add Treehub mock and uncomment the following tests
//TEST_F(OSTreeTest, Pull) {
//  ASSERT_TRUE(isRepoInited());
//  repo_->addRemote("treehub", "http://localhost:8787", "", "", "");
//  repo_->pull("treehub", "test", "7b5019ad0a1021e0368226844409f5015c1101b1370af2cc56e963f8d3e4f0cd");
//}

//TEST_F(OSTreeTest, Checkout) {
//  ASSERT_TRUE(isRepoInited());
//  const std::string commit_hash{"da7751c062967482bd7ac4b4d03f3c921d201e5ebc5f7d66449a5a59769d2384"};
//  repo_->addRemote("treehub", "http://localhost:8787", "", "", "");
//  repo_->pull("treehub", "test", commit_hash);
//  const std::string dir_to_checkout_to = mkdtemp(const_cast<char*>((testing::TempDir() + "OSTreeTest-folder-XXXXXX").c_str()));
//  try {
//    repo_->checkout(commit_hash, "/", dir_to_checkout_to);
//  } catch (const std::exception& exc) {

//  }

//  const auto expected_file = std::string(dir_to_checkout_to) + "/test.file";
//  ASSERT_TRUE(boost::filesystem::exists(expected_file));

//  boost::filesystem::remove_all(dir_to_checkout_to);
//}

int main(int argc, char **argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
