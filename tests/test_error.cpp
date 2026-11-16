// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <array>
#include <type_traits>
#include <utility>

#include "liquid/error.hpp"

TEST(Error, EveryCodeHasAName) {
    static_assert(std::is_aggregate_v<liquid::error>);
    constexpr std::array cases{
        std::pair{liquid::errc::ok, "ok"},
        std::pair{liquid::errc::not_found, "not_found"},
        std::pair{liquid::errc::already_exists, "already_exists"},
        std::pair{liquid::errc::conflict, "conflict"},
        std::pair{liquid::errc::invalid_argument, "invalid_argument"},
        std::pair{liquid::errc::unsupported, "unsupported"},
        std::pair{liquid::errc::corrupt, "corrupt"},
        std::pair{liquid::errc::io, "io"},
        std::pair{liquid::errc::locked, "locked"},
        std::pair{liquid::errc::unauthorized, "unauthorized"},
        std::pair{liquid::errc::unavailable, "unavailable"},
        std::pair{liquid::errc::busy, "busy"},
        std::pair{liquid::errc::not_configured, "not_configured"},
        std::pair{liquid::errc::internal, "internal"},
        std::pair{liquid::errc::not_remote, "not_remote"},
    };

    for (const auto& [code, name] : cases)
        EXPECT_EQ(liquid::to_string(code), name);
}

TEST(Error, UnknownCodeHasAStableName) {
    EXPECT_EQ(liquid::to_string(static_cast<liquid::errc>(65535)), "unknown");
}
