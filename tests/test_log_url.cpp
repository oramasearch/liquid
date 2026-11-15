// SPDX-License-Identifier: MIT

#include <string>
#include <utility>

#include <gtest/gtest.h>

#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

namespace {

using liquid::transparency::http_log_source;

struct invalid_url {
    const char* url;
    const char* diagnostic;
};

constexpr invalid_url invalid_urls[] = {
    {"not-a-url", "http:// or https://"},
    {"ftp://127.0.0.1", "http:// or https://"},
    {"http:/127.0.0.1", "http:// or https://"},
    {"https//127.0.0.1", "http:// or https://"},
    {"http://", "host"},
    {"https://", "host"},
    {"http:///log", "host"},
    {"https://:443/log", "host"},
    {"http://[]:80/log", "host"},
    {"http://?query", "host"},
    {"https://#fragment", "host"},
    {"http://127.0.0.1:", "port"},
    {"https://[::1]:", "port"},
    {"http://127.0.0.1:http", "port"},
    {"https://127.0.0.1:443x", "port"},
    {"http://127.0.0.1:0", "port"},
    {"https://127.0.0.1:65536", "port"},
    {"http://127.0.0.1:-1", "port"},
    {"http://127.0.0.1:+80", "port"},
    {"http://127.0.0.1:999999999999999999999999", "port"},
};

template <class T> void expect_invalid(const liquid::result<T>& value, const invalid_url& input) {
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().code, liquid::errc::invalid_argument);
    EXPECT_NE(value.error().message.find(input.diagnostic), std::string::npos);
    EXPECT_NE(value.error().message.find("HTTP"), std::string::npos);
    EXPECT_EQ(value.error().context, "transparency");
}

TEST(HttpLogSource, FactoryRejectsInvalidUrls) {
    for (const auto& input : invalid_urls) {
        SCOPED_TRACE(input.url);
        EXPECT_NO_THROW(expect_invalid(http_log_source::create(input.url), input));
    }
}

auto check_legacy_requests() -> liquid::runtime::task<void> {
    for (const auto& input : invalid_urls) {
        SCOPED_TRACE(input.url);
        // Every operation must return the retained parser error before transport access.
        auto source = http_log_source{input.url};
        EXPECT_EQ(source.name(), input.url);
        expect_invalid(co_await source.identity(), input);
        expect_invalid(co_await source.configuration(), input);
        expect_invalid(co_await source.current_checkpoint(), input);
        expect_invalid(co_await source.get_tile(0, 0, 1), input);
        expect_invalid(co_await source.get_inclusion_proof(0, 1), input);
        expect_invalid(co_await source.get_consistency_proof(1, 2), input);
    }
    co_return {};
}

TEST(HttpLogSource, LegacyConstructionReturnsErrorsFromEveryRequest) {
    const auto checked = liquid::runtime::block_on(check_legacy_requests());
    ASSERT_TRUE(checked) << checked.error().message;
}

TEST(HttpLogSource, FactoryAcceptsValidUrlsWithoutConnecting) {
    for (const auto* url : {"http://127.0.0.1",
                            "https://example.invalid/log/",
                            "http://127.0.0.1:1",
                            "https://127.0.0.1:65535/log",
                            "http://[::1]:8080/log"}) {
        SCOPED_TRACE(url);
        auto source = http_log_source::create(url);
        ASSERT_TRUE(source) << source.error().message;
        auto moved = std::move(*source);
        EXPECT_EQ(moved.name(), url);
    }
}

} // namespace
