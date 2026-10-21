// HTTP/1.1: the request parser liquid's servers use (Boost.HTTP, configured as src/http1/server.cpp
// configures it), fed in pieces as a socket would deliver them, and liquid's own response parser
// in the HTTP/1.1 client, including chunked transfer decoding.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string>

#include <boost/http/request_parser.hpp>

#include "fuzz.hpp"
#include "http1/client.hpp"

namespace {

void parse_request(std::span<const std::byte> input) {
    if (input.empty()) {
        return;
    }
    // The first byte sets the largest piece handed to the parser, from 1 to 256 bytes.
    const auto piece = std::to_integer<std::size_t>(input.front()) + 1;
    input = input.subspan(1);
    auto options = boost::http::parser_config{true};
    options.body_limit = 1024U * 1024U;
    static const auto config = boost::http::make_parser_config(options);
    auto parser = boost::http::request_parser{config};
    parser.reset();
    parser.start();
    // The same loop as boost::http::parser::read, with the input in place of a socket.
    auto ended = false;
    for (auto round = 0; round < 1'000'000; ++round) {
        auto error = boost::system::error_code{};
        parser.parse(error);
        if (parser.is_complete()) {
            return;
        }
        if (error && error != boost::http::condition::need_more_input) {
            return;
        }
        if (error != boost::http::condition::need_more_input) {
            continue;
        }
        if (input.empty()) {
            if (ended) {
                return;
            }
            parser.commit_eof();
            ended = true;
            continue;
        }
        auto buffer = *parser.prepare().begin();
        const auto count = std::min({buffer.size(), input.size(), piece});
        std::memcpy(buffer.data(), input.data(), count);
        parser.commit(count);
        input = input.subspan(count);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 2);
    if (!input) {
        return 0;
    }
    if (input->decoder == 0) {
        parse_request(input->payload);
    } else {
        (void)liquid::http1::parse_response(std::string{liquid::fuzz::text(input->payload)});
    }
    return 0;
}
