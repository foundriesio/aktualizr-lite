#include <gtest/gtest.h>

#include "boost/format.hpp"

#include "docker/docker.h"

TEST(Docker, ParseUri) {
  const std::string host{"host"};
  const std::string factory{"factory"};
  const std::string app{"app"};
  const std::string hash{"b0150d88116219cbf46ebb5dc08d8a559c4f1ab2731a788628fc7375b2372cb0"};

  {
    // regular Compose App hosted in Fio Registry
    const Docker::Uri uri{
        Docker::Uri::parseUri(boost::str(boost::format("%s/%s/%s@sha256:%s") % host % factory % app % hash))};
    ASSERT_EQ(uri.registryHostname, host);
    ASSERT_EQ(uri.factory, factory);
    ASSERT_EQ(uri.app, app);
    ASSERT_EQ(uri.digest.hash(), hash);
  }

  {
    // regular Compose App hosted in Fio Registry, hostname includes port
    const std::string host{"host:8080"};
    const Docker::Uri uri{
        Docker::Uri::parseUri(boost::str(boost::format("%s/%s/%s@sha256:%s") % host % factory % app % hash))};
    ASSERT_EQ(uri.registryHostname, host);
    ASSERT_EQ(uri.factory, factory);
    ASSERT_EQ(uri.app, app);
    ASSERT_EQ(uri.digest.hash(), hash);
  }

  {
    // image hosted at 3rd party Registry, image name contains just one element
    const std::string name{"alpine"};
    const Docker::Uri uri{
        Docker::Uri::parseUri(boost::str(boost::format("%s/%s@sha256:%s") % host % name % hash), false)};
    ASSERT_EQ(uri.registryHostname, host);
    ASSERT_EQ(uri.repo, name);
    ASSERT_EQ(uri.app, name);
    ASSERT_EQ(uri.factory.size(), 0);
    ASSERT_EQ(uri.digest.hash(), hash);
  }

  {
    // image hosted at 3rd party Registry, image name contains just one element. hostname includes port
    const std::string host{"host:8080"};
    const std::string name{"alpine"};
    const Docker::Uri uri{
        Docker::Uri::parseUri(boost::str(boost::format("%s/%s@sha256:%s") % host % name % hash), false)};
    ASSERT_EQ(uri.registryHostname, host);
    ASSERT_EQ(uri.repo, name);
    ASSERT_EQ(uri.app, name);
    ASSERT_EQ(uri.factory.size(), 0);
    ASSERT_EQ(uri.digest.hash(), hash);
  }

  {
    // image hosted at 3rd party Registry, image name contains two elements
    const std::string name{"library/alpine"};
    const Docker::Uri uri{
        Docker::Uri::parseUri(boost::str(boost::format("%s/%s@sha256:%s") % host % name % hash), false)};
    ASSERT_EQ(uri.registryHostname, host);
    ASSERT_EQ(uri.repo, name);
    ASSERT_EQ(uri.app, "alpine");
    ASSERT_EQ(uri.factory, "library");
    ASSERT_EQ(uri.digest.hash(), hash);
  }

  {
    // image hosted at 3rd party Registry, image name contains three elements
    const std::string name{"library/alpine/latest"};
    const Docker::Uri uri{
        Docker::Uri::parseUri(boost::str(boost::format("%s/%s@sha256:%s") % host % name % hash), false)};
    ASSERT_EQ(uri.registryHostname, host);
    ASSERT_EQ(uri.repo, name);
    ASSERT_EQ(uri.app, "latest");
    ASSERT_EQ(uri.factory, "library/alpine");
    ASSERT_EQ(uri.digest.hash(), hash);
  }
}

TEST(Docker, ParseUriNegative) {
  EXPECT_THROW(Docker::Uri::parseUri(""), std::invalid_argument);
  EXPECT_THROW(Docker::Uri::parseUri("foo"), std::invalid_argument);

  EXPECT_THROW(Docker::Uri::parseUri("host/factory/app@"), std::invalid_argument);
  EXPECT_THROW(Docker::Uri::parseUri("host/factory/app@sha256"), std::invalid_argument);
  EXPECT_THROW(Docker::Uri::parseUri("host/factory/app@sha256:"), std::invalid_argument);
  EXPECT_THROW(Docker::Uri::parseUri("host/factory/app@sha256:131313"), std::invalid_argument);

  EXPECT_THROW(Docker::Uri::parseUri("no-path@sha256:b0150d88116219cbf46ebb5dc08d8a559c4f1ab2731a788628fc7375b2372cb0"),
               std::invalid_argument);
  EXPECT_THROW(
      Docker::Uri::parseUri("host/no-factory@sha256:b0150d88116219cbf46ebb5dc08d8a559c4f1ab2731a788628fc7375b2372cb0"),
      std::invalid_argument);
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
