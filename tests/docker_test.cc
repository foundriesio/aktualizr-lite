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

TEST(Docker, BearerAuth) {
  {
    const auto auth{
        Docker::RegistryClient::BearerAuth("bearer "
                                           "realm=\"https://hub-auth.foundries.io/token-auth/"
                                           "\",service=\"registry\",scope=\"repository:msul-dev01/simpleapp:pull\"")};
    ASSERT_EQ(auth.Realm, "https://hub-auth.foundries.io/token-auth/");
    ASSERT_EQ(auth.Service, "registry");
    ASSERT_EQ(auth.Scope, "repository:msul-dev01/simpleapp:pull");
    ASSERT_EQ(auth.uri(),
              "https://hub-auth.foundries.io/token-auth/?service=registry&scope=repository:msul-dev01/simpleapp:pull");
  }
  {
    // correct but with spaces value of `www-authenticate`
    const auto auth{
        Docker::RegistryClient::BearerAuth("bearer   realm = \"https://hub-auth.foundries.io/token-auth/\" , service=  "
                                           " \"registry\" , scope  = \" repository:msul-dev01/simpleapp:pull,push\" ")};
    ASSERT_EQ(auth.Realm, "https://hub-auth.foundries.io/token-auth/");
    ASSERT_EQ(auth.Service, "registry");
    ASSERT_EQ(auth.Scope, "repository:msul-dev01/simpleapp:pull,push");
    ASSERT_EQ(
        auth.uri(),
        "https://hub-auth.foundries.io/token-auth/?service=registry&scope=repository:msul-dev01/simpleapp:pull,push");
  }
}

TEST(Docker, BearerAuthNegative) {
  {
    // unsupported auth type
    EXPECT_THROW(Docker::RegistryClient::BearerAuth("basic"), std::invalid_argument);
  }
  {
    // no required auth param
    EXPECT_THROW(
        Docker::RegistryClient::BearerAuth("bearer "
                                           "norealm=\"https://hub-auth.foundries.io/token-auth/"
                                           "\",service=\"registry\",scope=\"repository:msul-dev01/simpleapp:pull\""),
        std::invalid_argument);
    EXPECT_THROW(
        Docker::RegistryClient::BearerAuth("bearer "
                                           "realm=\"https://hub-auth.foundries.io/token-auth/"
                                           "\",service=\"registry\",noscope=\"repository:msul-dev01/simpleapp:pull\""),
        std::invalid_argument);
    EXPECT_THROW(
        Docker::RegistryClient::BearerAuth("bearer "
                                           "realm=\"https://hub-auth.foundries.io/token-auth/"
                                           "\",noservice=\"registry\",scope=\"repository:msul-dev01/simpleapp:pull\""),
        std::invalid_argument);
    EXPECT_THROW(Docker::RegistryClient::BearerAuth("bearer "), std::invalid_argument);
  }
  {
    // no `"`
    EXPECT_THROW(Docker::RegistryClient::BearerAuth("bearer realm =https://hub-auth.foundries.io/token-auth/"),
                 std::invalid_argument);
    // the opening `"` before `=`
    EXPECT_THROW(
        Docker::RegistryClient::BearerAuth("bearer realm\" "
                                           "=https://hub-auth.foundries.io/token-auth/"
                                           "\",service=\"registry\",scope=\"repository:msul-dev01/simpleapp:pull\""),
        std::invalid_argument);
    // the closing `"` is missing
    EXPECT_THROW(Docker::RegistryClient::BearerAuth("bearer realm=\"https://hub-auth.foundries.io/token-auth/"),
                 std::invalid_argument);
    // no opening `"` after `=`
    EXPECT_THROW(
        Docker::RegistryClient::BearerAuth("bearer realm = "
                                           "https://hub-auth.foundries.io/token-auth/"
                                           "\",service=\"registry\",scope=\"repository:msul-dev01/simpleapp:pull\""),
        std::invalid_argument);
  }
}

class ImageTest : virtual public ::testing::Test {
 protected:
  void SetUp() override {
    img_man_["mediaType"] = Docker::ImageManifest::Format;
    img_man_["schemaVersion"] = Docker::ImageManifest::Version;
    img_man_["config"]["mediaType"] = "application/vnd.docker.container.image.v1+json";
    img_man_["config"]["size"] = 6541;
    img_man_["config"]["digest"] = "sha256:99ae753c80968a7d7846dfbd06f0f0f7a425575955a275c7db01d0e9e34cab70";

    img_layers_[0]["mediaType"] = "application/vnd.docker.image.rootfs.diff.tar.gzip";
    img_layers_[0]["size"] = 1342;
    img_layers_[0]["digest"] = "sha256:48ecbb6b270eb481cb6df2a5b0332de294ec729e1968e92d725f1329637ce01b";
    img_layers_[1]["mediaType"] = "application/vnd.docker.image.rootfs.diff.tar.gzip";
    img_layers_[1]["size"] = 308482;
    img_layers_[1]["digest"] = "sha256:692f29ee68fa6bab04aa6a1c6d8db0ad44e287e5ff5c7e1d5794c3aabc55884d";
    img_man_["layers"] = img_layers_;
  }

  Json::Value img_man_;
  Json::Value img_layers_;
};

TEST_F(ImageTest, ImageManifest) {
  ASSERT_NO_THROW(const auto man{Docker::ImageManifest{img_man_}};
                  EXPECT_EQ(Docker::Descriptor{img_man_["config"]}, man.config()); const auto man_layers{man.layers()};
                  int indx{0}; for (const auto& l
                                    : man.layers()) { EXPECT_EQ(Docker::Descriptor{img_layers_[indx++]}, l); };);
}

TEST_F(ImageTest, ImageManifestNegative) {
  {
    // missing field
    Json::Value image_manifest;
    image_manifest["schemaVersion"] = Docker::ImageManifest::Version;
    ASSERT_THROW(Docker::ImageManifest{image_manifest}, std::runtime_error);
  }
  {
    // invalid config digest
    Json::Value image_manifest{img_man_};
    image_manifest["config"]["digest"] = "0968a7d7846dfbd06f0f0f7a425575955a275c7db01d0e9e34cab70";
    ASSERT_THROW(Docker::ImageManifest{image_manifest}.config(), std::invalid_argument);
  }
  {
    // invalid layer size
    Json::Value image_manifest{img_man_};
    image_manifest["layers"][0]["size"] = "foobar";
    ASSERT_THROW(Docker::ImageManifest{image_manifest}.layers(), Json::LogicError);
  }
}

int main(int argc, char** argv) {
  ::testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
