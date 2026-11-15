#include <gtest/gtest.h>

#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <cstdlib>
#include <string>
#include <thread>

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

// L-24 (liquid#436): an answer larger than the request's bound is an error, not an unbounded buffer.
TEST(PlaintextHttp, AnAnswerPastTheResponseBoundIsRefused) {
    const int listener = ::socket(AF_INET, SOCK_STREAM, 0);
    ASSERT_GE(listener, 0);
    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    ASSERT_EQ(::bind(listener, reinterpret_cast<sockaddr*>(&address), sizeof(address)), 0);
    ASSERT_EQ(::listen(listener, 1), 0);
    socklen_t length = sizeof(address);
    ASSERT_EQ(::getsockname(listener, reinterpret_cast<sockaddr*>(&address), &length), 0);
    std::thread server([listener] {
        const int client = ::accept(listener, nullptr, nullptr);
        if (client < 0) return;
        char request[4096];
        [[maybe_unused]] const auto received = ::recv(client, request, sizeof(request), 0);
        const std::string reply =
            "HTTP/1.1 200 OK\r\nConnection: close\r\n\r\n" + std::string(200U * 1024U, 'x');
        [[maybe_unused]] const auto sent = ::send(client, reply.data(), reply.size(), 0);
        ::close(client);
    });
    auto options = liquid::http1::request_options{
        .url = "http://127.0.0.1:" + std::to_string(ntohs(address.sin_port))};
    options.maximum_response_bytes = 1024;
    const auto answer = liquid::runtime::block_on(liquid::http1::request(std::move(options)));
    server.join();
    ::close(listener);
    ASSERT_FALSE(answer);
    EXPECT_NE(answer.error().message.find("larger than"), std::string::npos) << answer.error().message;
}
