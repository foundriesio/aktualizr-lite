#include <gtest/gtest.h>
#include "docker/composeinfo.h"
#include "logging/logging.h"
#include "utilities/utils.h"
#include "yaml2json.h"

class Yaml2JsonTest : public ::testing::Test {
 protected:
  Yaml2JsonTest() {}
};

TEST_F(Yaml2JsonTest, check_template) {
  try {
    Yaml2Json json("tests/template.yaml");
    ASSERT_EQ(json.root_["version"], "3.2");
    ASSERT_EQ(json.root_["services"]["dns64"]["image"], "hub.foundries.io/lmp/dns64:latest");
    ASSERT_EQ(json.root_["services"]["dns64"]["tmpfs"][1], "/var/lock");
  } catch (...) {
    ASSERT_TRUE(false);
  }
}

TEST_F(Yaml2JsonTest, compose_parser) {
  try {
    Docker::ComposeInfo parser("tests/template.yaml");

    // obtain all the services in the template file (we know there are 5)
    std::vector<Json::Value> services = parser.getServices();
    if (services.empty()) ASSERT_TRUE(false);

    // check all services's images are what we expect
    for (std::vector<Json::Value>::iterator it = services.begin(); it != services.end(); ++it) {
      std::string image = parser.getImage(*it);
      if (image.empty()) continue;

      Json::Value val = *it;
      if (val.asString() == "iface-mon-ot")
        ASSERT_EQ(image, "hub.foundries.io/lmp/iface-monitor:latest");
      else if (val.asString() == "ot-wpantund")
        ASSERT_EQ(image, "hub.foundries.io/lmp/ot-wpantund:latest");
      else if (val.asString() == "dns64")
        ASSERT_EQ(image, "hub.foundries.io/lmp/dns64:latest");
      else if (val.asString() == "jool")
        ASSERT_EQ(image, "hub.foundries.io/lmp/nat64-jool:latest");
      else if (val.asString() == "californium-proxy") {
        ASSERT_EQ(image, "hub.foundries.io/lmp/cf-proxy-coap-http:latest");
        std::string hash = parser.getHash(val);
        if (hash.empty()) ASSERT_TRUE(false);
        ASSERT_EQ(hash, "c675ec1bbcc2ac239611f5f6312538a5778d97cbdf6022581ab428425041cd69");
      } else
        ASSERT_TRUE(false);
    }
  } catch (...) {
    ASSERT_TRUE(false);
  }
}

TEST_F(Yaml2JsonTest, input_yaml_not_exist) {
  const auto yaml{"non-existing-file-001"};
  try {
    Yaml2Json json(yaml);
  } catch (const std::invalid_argument& exc) {
    ASSERT_EQ(exc.what(), std::string("The specified `yaml` file is not found: ") + yaml);
  } catch (const std::exception& exc) {
    FAIL() << "Expected `std::invalid_argument` got: " << typeid(exc).name();
  }
}

TEST_F(Yaml2JsonTest, input_yaml_empty) {
  TemporaryFile yaml{"foobar.yml"};
  yaml.PutContents("");
  try {
    Yaml2Json json(yaml.PathString());
  } catch (const std::invalid_argument& exc) {
    const std::string exc_msg{exc.what()};
    const std::string expected_msg{"Failed to parse the json representation of the input `yaml` file; path: " +
                                   yaml.PathString()};
    ASSERT_TRUE(0 == exc_msg.find(expected_msg)) << "Expected " << expected_msg << ", got " << exc.what();
  } catch (const std::exception& exc) {
    FAIL() << "Expected `std::invalid_argument` got: " << typeid(exc).name();
  }
}

TEST_F(Yaml2JsonTest, input_yaml_invalid) {
  TemporaryFile yaml{"foobar.yml"};
  yaml.PutContents("\t\t foobar:invalid:yaml:content \n{");
  try {
    Yaml2Json json(yaml.PathString());
  } catch (const std::invalid_argument& exc) {
    const std::string exc_msg{exc.what()};
    const std::string expected_msg{"Failed to parse the input `yaml` file; path: " + yaml.PathString()};
    ASSERT_TRUE(0 == exc_msg.find(expected_msg)) << "Expected " << expected_msg << ", got " << exc.what();
  } catch (const std::exception& exc) {
    FAIL() << "Expected `std::invalid_argument` got: " << typeid(exc).name();
  }
}

int main(int argc, char** argv) {
  testing::InitGoogleTest(&argc, argv);
  return RUN_ALL_TESTS();
}
