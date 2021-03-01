#include <gtest/gtest.h>

#include "target.h"

TEST(target, version) {
  ASSERT_TRUE(Target::Version("bar") < Target::Version("foo"));
  ASSERT_TRUE(Target::Version("1.bar") < Target::Version("2foo"));
  ASSERT_TRUE(Target::Version("1..0") < Target::Version("1.1"));
  ASSERT_TRUE(Target::Version("1.-1") < Target::Version("1.1"));
  ASSERT_TRUE(Target::Version("1.*bad #text") < Target::Version("1.1"));  // ord('*') < ord('1')
}

TEST(target, hasTag) {
  auto t = Uptane::Target::Unknown();

  // No tags defined in target:
  std::vector<std::string> config_tags;
  ASSERT_TRUE(Target::hasTag(t, config_tags));
  config_tags.push_back("foo");
  ASSERT_FALSE(Target::hasTag(t, config_tags));

  // Set target tags to: premerge, qa
  auto custom = t.custom_data();
  custom["tags"].append("premerge");
  custom["tags"].append("qa");
  t.updateCustom(custom);

  config_tags.clear();
  ASSERT_TRUE(Target::hasTag(t, config_tags));

  config_tags.push_back("qa");
  config_tags.push_back("blah");
  ASSERT_TRUE(Target::hasTag(t, config_tags));

  config_tags.clear();
  config_tags.push_back("premerge");
  ASSERT_TRUE(Target::hasTag(t, config_tags));

  config_tags.clear();
  config_tags.push_back("foo");
  ASSERT_FALSE(Target::hasTag(t, config_tags));
}

int main(int argc, char **argv) {
  ::testing::InitGoogleTest(&argc, argv);

  return RUN_ALL_TESTS();
}
