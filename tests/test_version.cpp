#include <gtest/gtest.h>

#include "liquid/version.hpp"

#include <string>

TEST(Version, StringMatchesStruct) {
    const auto v = liquid::current_version();
    const std::string expected =
        std::to_string(v.major) + "." + std::to_string(v.minor) + "." + std::to_string(v.patch);
    EXPECT_EQ(liquid::version_string(), expected);
    EXPECT_TRUE(liquid::grammar_digest().starts_with("sha256:"));
    EXPECT_EQ(liquid::grammar_digest().size(), 71U);
}

// The third place the version lives: project(liquid VERSION ...) in CMakeLists.txt.
TEST(Version, MatchesProjectVersion) {
    EXPECT_EQ(liquid::version_string(), std::string{LIQUID_PROJECT_VERSION});
}
