#include <gtest/gtest.h>

#include "liquid/liquid.hpp"

TEST(Hello, NamesTheVersion) {
    const std::string s = liquid::hello();
    EXPECT_NE(s.find("hello from liquid"), std::string::npos);
    EXPECT_NE(s.find(liquid::version_string()), std::string::npos);
}
