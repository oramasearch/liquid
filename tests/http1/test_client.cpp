// SPDX-License-Identifier: MIT

#include <string>

#include <gtest/gtest.h>

#include "http1/client.hpp"

namespace {

TEST(Http1Client, parses_length_and_chunked_responses) {
    const auto plain =
        liquid::http1::parse_response("HTTP/1.1 200 OK\r\nContent-Length: 2\r\n\r\nok");
    ASSERT_TRUE(plain) << plain.error().message;
    EXPECT_EQ(plain->status, 200U);
    EXPECT_EQ(plain->body.size(), 2U);
    const auto chunked = liquid::http1::parse_response(
        "HTTP/1.1 201 Created\r\nTransfer-Encoding: chunked\r\n\r\n2\r\nok\r\n0\r\n\r\n");
    ASSERT_TRUE(chunked) << chunked.error().message;
    EXPECT_EQ(chunked->status, 201U);
    EXPECT_EQ(chunked->body.size(), 2U);
}

// liquid #407: the fuzzer found that a malformed status line made Boost.HTTP throw out of
// parse_response, which returns a result. A server's bytes are untrusted, so it is now an error.
TEST(Http1Client, a_malformed_status_line_is_a_corrupt_response) {
    for (const auto* wire : {"HTTP/1.1 2x0 OK\r\n\r\n",
                             "\r\n\r\n",
                             "HTTP/9 200 OK\r\n\r\n",
                             "HTTP/1.1 200 OK\r\nbad header\r\n\r\n"}) {
        const auto parsed = liquid::http1::parse_response(wire);
        ASSERT_FALSE(parsed) << wire;
        EXPECT_EQ(parsed.error().code, liquid::errc::corrupt) << wire;
    }
}

} // namespace
