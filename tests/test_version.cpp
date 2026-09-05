#include <gtest/gtest.h>

#include "liquid/version.hpp"

TEST(Version, StringMatchesStruct) {
    const auto v = liquid::current_version();
    EXPECT_EQ(v.major, 0u);
    EXPECT_EQ(v.minor, 0u);
    EXPECT_EQ(v.patch, 1u);
    EXPECT_EQ(liquid::version_string(), "0.0.1");
    EXPECT_TRUE(liquid::grammar_digest().starts_with("sha256:"));
    EXPECT_EQ(liquid::grammar_digest().size(), 71U);
}
