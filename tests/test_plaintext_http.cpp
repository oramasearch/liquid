#include <gtest/gtest.h>

#include <cstdlib>
#include <string>

#include "http1/client.hpp"
#include "liquid/runtime.hpp"

// L-10 (liquid#392): a request that carries a credential or is marked sensitive goes over plain http
// only to loopback, an in-cluster Service, or with LIQUID_ALLOW_INSECURE_HTTP=1. Anywhere else it is
// refused before a name is resolved or a byte is sent.
TEST(PlaintextHttp, SensitiveRequestsStayOffThePublicNetwork) {
    ::unsetenv("LIQUID_ALLOW_INSECURE_HTTP");
    EXPECT_TRUE(liquid::http1::plaintext_allowed("localhost"));
    EXPECT_TRUE(liquid::http1::plaintext_allowed("127.0.0.1"));
    EXPECT_TRUE(liquid::http1::plaintext_allowed("::1"));
    EXPECT_TRUE(liquid::http1::plaintext_allowed("lake-prod.lake.svc.cluster.local"));
    EXPECT_TRUE(liquid::http1::plaintext_allowed("lake-prod.lake.svc"));
    EXPECT_FALSE(liquid::http1::plaintext_allowed("lake.example.test"));
    EXPECT_FALSE(liquid::http1::plaintext_allowed("svc.example.test"));

    for (const bool by_header : {true, false}) {
        auto options = liquid::http1::request_options{.url = "http://lake.example.test"};
        if (by_header) {
            options.headers.emplace_back("Authorization", "Bearer must-not-travel");
        } else {
            options.sensitive = true;
        }
        const auto answer = liquid::runtime::block_on(liquid::http1::request(std::move(options)));
        ASSERT_FALSE(answer);
        EXPECT_EQ(answer.error().code, liquid::errc::invalid_argument);
        EXPECT_NE(answer.error().message.find("refusing plain http"), std::string::npos);
    }

    ::setenv("LIQUID_ALLOW_INSECURE_HTTP", "1", 1);
    EXPECT_TRUE(liquid::http1::plaintext_allowed("lake.example.test"));
    ::unsetenv("LIQUID_ALLOW_INSECURE_HTTP");
}
