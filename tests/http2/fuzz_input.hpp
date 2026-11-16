#pragma once

// One HTTP/2 fuzz input: arbitrary bytes from a peer, after the client preface, served by a
// connection that reads requests and answers them the way `liquid serve reapi` does. Shared by the
// libFuzzer harness (tests/http2/fuzz_connection.cpp) and the seeded corpus test in
// test_limits.cpp, so the harness's code path also runs in every ctest.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>

#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/test/run_blocking.hpp>
#include <boost/capy/test/stream.hpp>

#include "http2/connection.hpp"

namespace liquid::http2::fuzz {

inline auto serve(connection& server) -> runtime::task<void> {
    auto started = co_await server.start();
    if (!started) {
        co_return std::unexpected(started.error());
    }
    for (;;) {
        auto stream_id = co_await server.receive_stream();
        if (!stream_id) {
            co_return std::unexpected(stream_id.error());
        }
        auto headers = co_await server.receive_headers(*stream_id);
        if (!headers) {
            co_return std::unexpected(headers.error());
        }
        auto body = co_await server.receive_data(*stream_id);
        if (!body) {
            co_return std::unexpected(body.error());
        }
        const auto response = std::array{header_field{":status", "200", false}};
        auto sent = co_await server.send_headers(*stream_id, response, false);
        if (!sent) {
            co_return std::unexpected(sent.error());
        }
        auto echoed = co_await server.send_data(*stream_id, std::move(*body), false);
        if (!echoed) {
            co_return std::unexpected(echoed.error());
        }
        const auto trailers = std::array{header_field{"grpc-status", "0", false}};
        auto ended = co_await server.send_headers(*stream_id, trailers, true);
        if (!ended) {
            co_return std::unexpected(ended.error());
        }
    }
}

// Runs one input to completion. The peer's end closes after the input, so every read ends.
inline void run_input(std::span<const std::uint8_t> input) {
    auto [server_end, peer] = boost::capy::test::make_stream_pair();
    auto bytes = std::string{"PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n"};
    bytes.append(reinterpret_cast<const char*>(input.data()), input.size());
    peer.provide(bytes);
    peer.close();
    auto stream = boost::capy::any_stream{&server_end};
    auto options = connection_options{};
    options.initial_window_size = 4096;
    auto server = connection{stream, endpoint_role::server, options};
    boost::capy::test::run_blocking([](result<void>) {})(serve(server));
}

} // namespace liquid::http2::fuzz
