// SPDX-License-Identifier: MIT

#pragma once

#include <gtest/gtest.h>

#include <string_view>
#include <utility>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

template <class T>
void expect_unsupported(const liquid::result<T>& value, std::string_view context) {
    ASSERT_FALSE(value.has_value());
    EXPECT_EQ(value.error().code, liquid::errc::unsupported);
    EXPECT_EQ(value.error().context, context);
}

template <class T>
void expect_unsupported(liquid::runtime::task<T> value, std::string_view context) {
    expect_unsupported(liquid::runtime::block_on(std::move(value)), context);
}
