#include <gtest/gtest.h>
#include <boost/filesystem.hpp>
#include <boost/algorithm/string.hpp>

#include "ostree/repo.h"
#include "test_utils.h"
#include "utilities/utils.h"

#include "fixtures/liteclient/executecmd.cc"
#include "fixtures/liteclient/ostreerepomock.cc"


class OSTreeTest : public ::testing::Test {
 protected:
  OSTreeTest():path_{test_dir_.PathString()}, repo_{std::make_unique<OSTree::Repo>(path_, true)}, repo_mock_{path_} {}
  bool isRepoInited() const {
    return boost::filesystem::exists(path_ + "/config") && boost::filesystem::exists(path_ + "/objects");
  }

 protected:
  TemporaryDirectory test_dir_;  // must be the first element in the class
  const std::string path_;
  std::unique_ptr<OSTree::Repo> repo_;
  OSTreeRepoMock repo_mock_;
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

TEST_F(OSTreeTest, ReadFileFromCommit) {
  ASSERT_TRUE(isRepoInited());
  const std::string content_dir{"contentdir"};
  const std::string file_content{"foobar=100"};
  const std::string file_name{"version.txt"};

  {
    // non-existing commit hash
    EXPECT_THROW(repo_->readFile("7b5019ad0a1021e0368226844409f5015c1101b1370af2cc56e963f8d3e4f0cd", file_name), std::runtime_error);
  }
  {
    // non-existing file
    Utils::writeFile(test_dir_ / content_dir / file_name, file_content, true);
    const auto commit_hash{repo_mock_.commit((test_dir_ / "contentdir").string(), "lmp")};
    EXPECT_THROW(repo_->readFile(commit_hash, "nonexistingfile"), std::runtime_error);
  }

  // positive cases for empty, small and bigger file
  std::string big_file_content;
  for (int ii = 0; ii < 1024; ++ii) {
    big_file_content.append(Utils::randomUuid());
  }
  for (const auto& file: std::list<std::string>{file_content, "", big_file_content}) {
    // empty file
    Utils::writeFile(test_dir_ / "contentdir" / file_name, file, true);
    const auto commit_hash{repo_mock_.commit((test_dir_ / "contentdir").string(), "lmp")};
    const auto res_file_content{repo_->readFile(commit_hash, file_name)};
    ASSERT_EQ(res_file_content.size(), file.size());
    ASSERT_EQ(res_file_content, file);
  }
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
