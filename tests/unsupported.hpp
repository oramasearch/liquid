#pragma once

#include <gtest/gtest.h>

#include <string_view>

#include "liquid/error.hpp"

template <class T>
void expect_unsupported(const liquid::result<T>& value, std::string_view context) {
    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().code, liquid::errc::unsupported);
    EXPECT_EQ(value.error().context, context);
}
