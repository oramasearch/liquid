// SPDX-License-Identifier: MIT

// Denial-of-service limits of the HTTP/2 connection (liquid#412, security review finding L-30).
//
// Each test plays a hostile peer: it writes raw frames into one end of an in-memory stream pair,
// runs a server connection on the other end until the input is exhausted, and reads back what the
// server wrote.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdint>
#include <functional>
#include <optional>
#include <random>
#include <string>
#include <string_view>
#include <vector>

#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/test/run_blocking.hpp>
#include <boost/capy/test/stream.hpp>

#include "fuzz_input.hpp"
#include "http2/connection.hpp"
#include "http2/frame.hpp"
#include "http2/hpack.hpp"

namespace {

using liquid::http2::connection;
using liquid::http2::connection_options;
using liquid::http2::frame;
using liquid::http2::frame_type;
using liquid::http2::header_field;

constexpr auto flag_end_stream = std::uint8_t{0x1};
constexpr auto flag_ack = std::uint8_t{0x1};
constexpr auto flag_end_headers = std::uint8_t{0x4};

constexpr auto code_protocol_error = std::uint32_t{0x1};
constexpr auto code_flow_control_error = std::uint32_t{0x3};
constexpr auto code_frame_size_error = std::uint32_t{0x6};
constexpr auto code_refused_stream = std::uint32_t{0x7};
constexpr auto code_compression_error = std::uint32_t{0x9};
constexpr auto code_enhance_your_calm = std::uint32_t{0xb};

auto u32(std::uint32_t value) -> std::vector<std::byte> {
    return {static_cast<std::byte>((value >> 24) & 0xffU),
            static_cast<std::byte>((value >> 16) & 0xffU),
            static_cast<std::byte>((value >> 8) & 0xffU),
            static_cast<std::byte>(value & 0xffU)};
}

auto read_u32(const std::vector<std::byte>& input, std::size_t offset) -> std::uint32_t {
    return (std::to_integer<std::uint32_t>(input[offset]) << 24) |
           (std::to_integer<std::uint32_t>(input[offset + 1]) << 16) |
           (std::to_integer<std::uint32_t>(input[offset + 2]) << 8) |
           std::to_integer<std::uint32_t>(input[offset + 3]);
}

// A hostile peer's byte stream, built frame by frame.
class peer_script {
public:
    peer_script() {
        bytes_ = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
    }

    auto add(frame_type type,
             std::uint8_t flags,
             std::uint32_t stream_id,
             std::vector<std::byte> payload = {}) -> peer_script& {
        const auto encoded =
            liquid::http2::encode_frame(frame{type, flags, stream_id, std::move(payload)});
        EXPECT_TRUE(encoded.has_value());
        for (const auto value : *encoded) {
            bytes_.push_back(static_cast<char>(std::to_integer<unsigned char>(value)));
        }
        return *this;
    }

    auto settings(std::vector<std::pair<std::uint16_t, std::uint32_t>> values = {})
        -> peer_script& {
        auto payload = std::vector<std::byte>{};
        for (const auto& [id, value] : values) {
            const auto setting = liquid::http2::setting_payload(id, value);
            payload.insert(payload.end(), setting.begin(), setting.end());
        }
        return add(frame_type::settings, 0, 0, std::move(payload));
    }

    auto headers(std::uint32_t stream_id, std::vector<header_field> fields, bool end_stream)
        -> peer_script& {
        auto encoded = encoder_.encode(fields);
        EXPECT_TRUE(encoded.has_value());
        return add(frame_type::headers,
                   static_cast<std::uint8_t>(flag_end_headers | (end_stream ? flag_end_stream : 0)),
                   stream_id,
                   std::move(*encoded));
    }

    auto request(std::uint32_t stream_id, bool end_stream = false) -> peer_script& {
        return headers(stream_id,
                       {{":method", "POST", false},
                        {":scheme", "http", false},
                        {":path", "/test", false},
                        {"content-type", "application/grpc", false}},
                       end_stream);
    }

    auto data(std::uint32_t stream_id, std::size_t size, bool end_stream) -> peer_script& {
        return add(frame_type::data,
                   end_stream ? flag_end_stream : 0,
                   stream_id,
                   std::vector<std::byte>(size, std::byte{'x'}));
    }

    auto reset(std::uint32_t stream_id) -> peer_script& {
        return add(frame_type::rst_stream, 0, stream_id, u32(0x8));
    }

    [[nodiscard]] auto bytes() const -> const std::string& {
        return bytes_;
    }

private:
    std::string bytes_;
    liquid::http2::hpack_encoder encoder_;
};

using server_body = std::function<liquid::runtime::task<void>(connection&)>;

struct outcome {
    liquid::result<void> result;
    std::vector<frame> written;
    std::size_t tracked_streams{};
    std::size_t pending_bytes{};
    std::size_t encoder_table_size{};
    std::size_t decoder_table_limit{};

    [[nodiscard]] auto goaway_code() const -> std::optional<std::uint32_t> {
        for (const auto& value : written) {
            if (value.type == frame_type::goaway && value.payload.size() >= 8) {
                return read_u32(value.payload, 4);
            }
        }
        return std::nullopt;
    }

    [[nodiscard]] auto resets(std::uint32_t code) const -> std::vector<std::uint32_t> {
        auto output = std::vector<std::uint32_t>{};
        for (const auto& value : written) {
            if (value.type == frame_type::rst_stream && read_u32(value.payload, 0) == code) {
                output.push_back(value.stream_id);
            }
        }
        return output;
    }

    [[nodiscard]] auto count(frame_type type, std::uint8_t flags) const -> std::size_t {
        auto total = std::size_t{};
        for (const auto& value : written) {
            total += value.type == type && value.flags == flags ? 1U : 0U;
        }
        return total;
    }

    [[nodiscard]] auto window_updates(std::uint32_t stream_id) const -> std::size_t {
        auto total = std::size_t{};
        for (const auto& value : written) {
            total += value.type == frame_type::window_update && value.stream_id == stream_id;
        }
        return total;
    }

    [[nodiscard]] auto first_settings() const
        -> std::vector<std::pair<std::uint16_t, std::uint32_t>> {
        for (const auto& value : written) {
            if (value.type != frame_type::settings || (value.flags & flag_ack) != 0) {
                continue;
            }
            auto output = std::vector<std::pair<std::uint16_t, std::uint32_t>>{};
            for (std::size_t offset = 0; offset + 6 <= value.payload.size(); offset += 6) {
                const auto id = static_cast<std::uint16_t>(
                    (std::to_integer<std::uint32_t>(value.payload[offset]) << 8) |
                    std::to_integer<std::uint32_t>(value.payload[offset + 1]));
                output.emplace_back(id, read_u32(value.payload, offset + 2));
            }
            return output;
        }
        return {};
    }
};

// Reads requests the way `liquid serve reapi` does, one stream at a time, until the input ends.
auto accept_streams(connection& server) -> liquid::runtime::task<void> {
    for (;;) {
        auto stream_id = co_await server.receive_stream();
        if (!stream_id) {
            co_return std::unexpected(stream_id.error());
        }
    }
}

auto run_server(const peer_script& script, connection_options options, server_body body)
    -> outcome {
    auto [server_end, peer] = boost::capy::test::make_stream_pair();
    peer.provide(script.bytes());
    peer.close();
    auto stream = boost::capy::any_stream{&server_end};
    auto server = connection{stream, liquid::http2::endpoint_role::server, options};
    auto output = outcome{};
    auto session = [](connection& server, server_body body) -> liquid::runtime::task<void> {
        auto started = co_await server.start();
        if (!started) {
            co_return std::unexpected(started.error());
        }
        co_return co_await body(server);
    };
    boost::capy::test::run_blocking([&](liquid::result<void> value) {
        output.result = std::move(value);
    })(session(server, std::move(body)));
    output.tracked_streams = server.tracked_streams();
    output.pending_bytes = server.pending_bytes();
    output.encoder_table_size = server.encoder_table_size();
    output.decoder_table_limit = server.decoder_table_limit();

    auto raw = std::vector<std::byte>{};
    for (const auto character : peer.data()) {
        raw.push_back(static_cast<std::byte>(static_cast<unsigned char>(character)));
    }
    for (std::size_t offset = 0; offset + 9 <= raw.size();) {
        const auto length = (std::to_integer<std::size_t>(raw[offset]) << 16) |
                            (std::to_integer<std::size_t>(raw[offset + 1]) << 8) |
                            std::to_integer<std::size_t>(raw[offset + 2]);
        const auto decoded = liquid::http2::decode_frame(
            std::span<const std::byte>(raw.data() + offset, 9 + length));
        EXPECT_TRUE(decoded.has_value());
        if (!decoded) {
            break;
        }
        output.written.push_back(*decoded);
        offset += 9 + length;
    }
    return output;
}

auto run_server(const peer_script& script, connection_options options = {}) -> outcome {
    return run_server(script, options, accept_streams);
}

auto hpack_integer(std::vector<std::byte>& output,
                   std::uint8_t prefix,
                   std::uint8_t first,
                   std::size_t value) {
    const auto limit = (std::size_t{1} << prefix) - 1;
    if (value < limit) {
        output.push_back(static_cast<std::byte>(first | value));
        return;
    }
    output.push_back(static_cast<std::byte>(first | limit));
    value -= limit;
    while (value >= 128) {
        output.push_back(static_cast<std::byte>((value & 0x7fU) | 0x80U));
        value >>= 7;
    }
    output.push_back(static_cast<std::byte>(value));
}

auto hpack_literal(std::vector<std::byte>& output, std::string_view text) {
    hpack_integer(output, 7, 0x00, text.size());
    for (const auto character : text) {
        output.push_back(static_cast<std::byte>(character));
    }
}

TEST(Http2Limits, AdvertisesTheLimitsItEnforces) {
    auto script = peer_script{};
    script.settings();
    const auto result = run_server(script);
    const auto settings = result.first_settings();
    const auto has = [&](std::uint16_t id, std::uint32_t value) {
        return std::ranges::find(settings, std::pair{id, value}) != settings.end();
    };
    EXPECT_TRUE(has(0x1, 4096)) << "SETTINGS_HEADER_TABLE_SIZE";
    EXPECT_TRUE(has(0x3, 100)) << "SETTINGS_MAX_CONCURRENT_STREAMS";
    EXPECT_TRUE(has(0x6, 16 * 1024)) << "SETTINGS_MAX_HEADER_LIST_SIZE";
}

TEST(Http2Limits, RefusesStreamsOverMaxConcurrentStreams) {
    auto script = peer_script{};
    script.settings().request(1).request(3).request(5).request(7);
    auto options = connection_options{};
    options.max_concurrent_streams = 2;
    const auto result = run_server(script, options);
    EXPECT_EQ(result.resets(code_refused_stream), (std::vector<std::uint32_t>{5, 7}));
    EXPECT_EQ(result.tracked_streams, 2U);
    // A refused stream's header block is still decoded, so the next one decodes too.
    EXPECT_FALSE(result.goaway_code().has_value());
}

TEST(Http2Limits, PeerHeaderTableSizeLimitsTheEncoderNotTheDecoder) {
    auto script = peer_script{};
    script.settings({{0x1, 0xffff'ffffU}});
    script.add(frame_type::settings, flag_ack, 0);
    // A dynamic table size update far above the 4096 this endpoint advertised.
    auto block = std::vector<std::byte>{};
    hpack_integer(block, 5, 0x20, 1'000'000);
    block.push_back(std::byte{0x83}); // :method POST
    script.add(frame_type::headers, flag_end_headers, 1, block);
    const auto result = run_server(script);
    EXPECT_EQ(result.encoder_table_size, 4096U);
    EXPECT_EQ(result.decoder_table_limit, 4096U);
    EXPECT_EQ(result.goaway_code(), code_compression_error);
    EXPECT_FALSE(result.result.has_value());
}

TEST(Http2Limits, EncoderShrinksToThePeersHeaderTableSize) {
    auto script = peer_script{};
    script.settings({{0x1, 0}}).request(1, true);
    const auto result =
        run_server(script, {}, [](connection& server) -> liquid::runtime::task<void> {
            auto headers = co_await server.receive_headers(1);
            if (!headers) {
                co_return std::unexpected(headers.error());
            }
            const auto response = std::array{header_field{":status", "200", false}};
            co_return co_await server.send_headers(1, response, true);
        });
    ASSERT_TRUE(result.result.has_value()) << result.result.error().message;
    EXPECT_EQ(result.encoder_table_size, 0U);
    const auto response = std::ranges::find_if(
        result.written, [](const frame& value) { return value.type == frame_type::headers; });
    ASSERT_NE(response, result.written.end());
    ASSERT_FALSE(response->payload.empty());
    // The block opens with a dynamic table size update to zero.
    EXPECT_EQ(response->payload.front(), std::byte{0x20});
}

TEST(Http2Limits, RejectsFramesLargerThanItsOwnMaxFrameSize) {
    auto script = peer_script{};
    // The peer may receive large frames; that does not let it send them.
    script.settings({{0x5, 16'777'215U}}).request(1).data(1, 20'000, true);
    const auto result =
        run_server(script, {}, [](connection& server) -> liquid::runtime::task<void> {
            auto headers = co_await server.receive_headers(1);
            if (!headers) {
                co_return std::unexpected(headers.error());
            }
            auto body = co_await server.receive_data(1);
            if (!body) {
                co_return std::unexpected(body.error());
            }
            co_return {};
        });
    EXPECT_EQ(result.goaway_code(), code_frame_size_error);
    EXPECT_FALSE(result.result.has_value());
}

TEST(Http2Limits, ClosesOnContinuationFlood) {
    auto script = peer_script{};
    script.settings();
    auto block = std::vector<std::byte>{std::byte{0x83}};
    script.add(frame_type::headers, 0, 1, block);
    for (auto index = 0; index < 40; ++index) {
        script.add(frame_type::continuation, 0, 1, std::vector<std::byte>(1000, std::byte{0x83}));
    }
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
    EXPECT_EQ(result.result.error().code, liquid::errc::busy);
}

TEST(Http2Limits, ClosesOnEmptyContinuationFlood) {
    auto script = peer_script{};
    script.settings();
    script.add(frame_type::headers, 0, 1, {std::byte{0x83}});
    for (auto index = 0; index < 1000; ++index) {
        script.add(frame_type::continuation, 0, 1);
    }
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
}

TEST(Http2Limits, RejectsAHeaderBlockInterruptedByAnotherFrame) {
    auto script = peer_script{};
    script.settings();
    script.add(frame_type::headers, 0, 1, {std::byte{0x83}});
    script.add(frame_type::headers, flag_end_headers, 3, {std::byte{0x83}});
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_protocol_error);
}

TEST(Http2Limits, CapsTheDecodedHeaderListSize) {
    auto script = peer_script{};
    script.settings();
    // One 4000-byte entry in the dynamic table, then ten one-byte references to it: 21 bytes on
    // the wire that would decode to 40 KB of headers.
    auto block = std::vector<std::byte>{std::byte{0x40}};
    hpack_literal(block, "x-big");
    hpack_literal(block, std::string(4000, 'a'));
    for (auto index = 0; index < 10; ++index) {
        block.push_back(std::byte{0xbe}); // dynamic index 62
    }
    script.add(frame_type::headers, flag_end_headers, 1, block);
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
}

TEST(Http2Limits, ClosesOnRapidReset) {
    auto script = peer_script{};
    script.settings();
    for (std::uint32_t stream_id = 1; stream_id < 2 * 400; stream_id += 2) {
        script.request(stream_id).reset(stream_id);
    }
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
    EXPECT_EQ(result.result.error().code, liquid::errc::busy);
    EXPECT_LE(result.tracked_streams, 1U);
}

TEST(Http2Limits, ForgetsStreamsThePeerResets) {
    auto script = peer_script{};
    script.settings();
    for (std::uint32_t stream_id = 1; stream_id < 2 * 50; stream_id += 2) {
        script.request(stream_id).data(stream_id, 100, false).reset(stream_id);
    }
    const auto result = run_server(script);
    EXPECT_FALSE(result.goaway_code().has_value());
    EXPECT_EQ(result.tracked_streams, 0U);
    EXPECT_EQ(result.pending_bytes, 0U);
}

TEST(Http2Limits, ClosesOnPingFlood) {
    auto script = peer_script{};
    script.settings();
    for (auto index = 0; index < 500; ++index) {
        script.add(frame_type::ping, 0, 0, std::vector<std::byte>(8));
    }
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
    EXPECT_LE(result.count(frame_type::ping, flag_ack), 60U);
}

TEST(Http2Limits, ClosesOnSettingsFlood) {
    auto script = peer_script{};
    for (auto index = 0; index < 500; ++index) {
        script.settings();
    }
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
    EXPECT_LE(result.count(frame_type::settings, flag_ack), 60U);
}

TEST(Http2Limits, ClosesOnEmptyDataFlood) {
    auto script = peer_script{};
    script.settings().request(1);
    for (auto index = 0; index < 1000; ++index) {
        script.data(1, 0, false);
    }
    const auto result = run_server(script);
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
}

// The caller reads stream 1 while the peer sends DATA on stream 3. Stream 3's window is refilled
// only when the caller reads it, so the peer cannot queue more than one window of unread data.
TEST(Http2Limits, RefillsTheReceiveWindowOnlyAsDataIsRead) {
    auto script = peer_script{};
    script.settings().request(1).request(3).data(3, 1024, false).data(1, 10, true);
    auto options = connection_options{};
    options.initial_window_size = 1024;
    const auto result =
        run_server(script, options, [](connection& server) -> liquid::runtime::task<void> {
            auto headers = co_await server.receive_headers(1);
            if (!headers) {
                co_return std::unexpected(headers.error());
            }
            auto body = co_await server.receive_data(1);
            if (!body) {
                co_return std::unexpected(body.error());
            }
            co_return {};
        });
    ASSERT_TRUE(result.result.has_value()) << result.result.error().message;
    EXPECT_EQ(result.window_updates(3), 0U);
    // Stream 3's request and its unread DATA are still queued.
    EXPECT_GE(result.pending_bytes, 1024U);
}

TEST(Http2Limits, RejectsDataBeyondTheStreamWindow) {
    auto script = peer_script{};
    script.settings().request(1).request(3).data(3, 1024, false).data(3, 1024, false);
    auto options = connection_options{};
    options.initial_window_size = 1024;
    const auto result =
        run_server(script, options, [](connection& server) -> liquid::runtime::task<void> {
            auto headers = co_await server.receive_headers(1);
            if (!headers) {
                co_return std::unexpected(headers.error());
            }
            auto body = co_await server.receive_data(1);
            if (!body) {
                co_return std::unexpected(body.error());
            }
            co_return {};
        });
    EXPECT_EQ(result.goaway_code(), code_flow_control_error);
}

TEST(Http2Limits, CapsBytesQueuedForUnreadStreams) {
    auto script = peer_script{};
    script.settings().request(1);
    for (std::uint32_t stream_id = 3; stream_id < 3 + 2 * 8; stream_id += 2) {
        script.request(stream_id).data(stream_id, 1024, false);
    }
    auto options = connection_options{};
    options.initial_window_size = 1024;
    options.max_pending_bytes = 4096;
    const auto result =
        run_server(script, options, [](connection& server) -> liquid::runtime::task<void> {
            auto headers = co_await server.receive_headers(1);
            if (!headers) {
                co_return std::unexpected(headers.error());
            }
            auto body = co_await server.receive_data(1);
            if (!body) {
                co_return std::unexpected(body.error());
            }
            co_return {};
        });
    EXPECT_EQ(result.goaway_code(), code_enhance_your_calm);
    EXPECT_LE(result.pending_bytes, 4096U);
}

// HPACK state follows the order blocks arrive in, not the order the caller reads streams in.
TEST(Http2Limits, DecodesHeaderBlocksInArrivalOrder) {
    auto script = peer_script{};
    script.settings();
    auto first = std::vector<std::byte>{std::byte{0x40}};
    hpack_literal(first, "x-a");
    hpack_literal(first, "one");
    script.add(frame_type::headers, flag_end_headers | flag_end_stream, 1, first);
    script.add(frame_type::headers, flag_end_headers | flag_end_stream, 3, {std::byte{0xbe}});
    auto third = std::optional<std::vector<header_field>>{};
    const auto result =
        run_server(script, {}, [&third](connection& server) -> liquid::runtime::task<void> {
            auto later = co_await server.receive_headers(3);
            if (!later) {
                co_return std::unexpected(later.error());
            }
            third = std::move(*later);
            auto earlier = co_await server.receive_headers(1);
            if (!earlier) {
                co_return std::unexpected(earlier.error());
            }
            co_return {};
        });
    ASSERT_TRUE(result.result.has_value()) << result.result.error().message;
    ASSERT_TRUE(third.has_value());
    EXPECT_EQ(*third, (std::vector<header_field>{{"x-a", "one", false}}));
}

TEST(Http2Hpack, DecoderRejectsHeaderListOverLimit) {
    auto encoder = liquid::http2::hpack_encoder{};
    auto decoder = liquid::http2::hpack_decoder{};
    const auto fields = std::array{header_field{"x-long", std::string(200, 'v'), false}};
    const auto block = encoder.encode(fields);
    ASSERT_TRUE(block.has_value());
    EXPECT_FALSE(decoder.decode(*block, 100).has_value());
    EXPECT_TRUE(decoder.decode(*block, 1000).has_value());
}

TEST(Http2Hpack, DecoderAcceptsSizeUpdatesUpToItsLimitOnly) {
    auto decoder = liquid::http2::hpack_decoder{4096};
    auto shrink = std::vector<std::byte>{};
    hpack_integer(shrink, 5, 0x20, 0);
    ASSERT_TRUE(decoder.decode(shrink).has_value());
    // The peer may grow the table again, up to the advertised limit and no further.
    auto grow = std::vector<std::byte>{};
    hpack_integer(grow, 5, 0x20, 4096);
    EXPECT_TRUE(decoder.decode(grow).has_value());
    auto beyond = std::vector<std::byte>{};
    hpack_integer(beyond, 5, 0x20, 4097);
    EXPECT_FALSE(decoder.decode(beyond).has_value());
}

// Runs the fuzz harness's entry point over a seeded corpus of mostly well-formed frames with
// random types, flags, stream identifiers and payloads, so that path is exercised in every ctest
// and not only under libFuzzer. It passes when every input finishes without a crash or a hang.
TEST(Http2Limits, SurvivesASeededRandomFrameCorpus) {
    auto generator = std::mt19937{412};
    const auto pick = [&generator](std::uint32_t bound) {
        return std::uniform_int_distribution<std::uint32_t>{0, bound - 1}(generator);
    };
    constexpr auto stream_ids = std::array<std::uint32_t, 7>{0, 1, 2, 3, 5, 7, 9};
    for (auto input_index = 0; input_index < 2000; ++input_index) {
        auto encoder = liquid::http2::hpack_encoder{};
        auto bytes = std::vector<std::byte>{};
        const auto append = [&bytes](const frame& value) {
            const auto encoded = liquid::http2::encode_frame(value);
            ASSERT_TRUE(encoded.has_value());
            bytes.insert(bytes.end(), encoded->begin(), encoded->end());
        };
        append(frame{frame_type::settings, 0, 0, {}});
        const auto frames = pick(40);
        for (std::uint32_t index = 0; index < frames; ++index) {
            auto value = frame{};
            value.type = static_cast<frame_type>(pick(11));
            value.flags = static_cast<std::uint8_t>(pick(2) == 0 ? pick(256) : flag_end_headers);
            value.stream_id = stream_ids[pick(stream_ids.size())];
            switch (value.type) {
            case frame_type::headers: {
                auto block = encoder.encode(std::array{
                    header_field{":path", "/p" + std::to_string(pick(5)), false},
                    header_field{
                        "x-" + std::to_string(pick(3)), std::string(pick(64), 'v'), pick(4) == 0}});
                value.payload = std::move(*block);
                break;
            }
            case frame_type::settings:
                value.payload.resize(6 * pick(4));
                break;
            case frame_type::window_update:
            case frame_type::rst_stream:
                value.payload.resize(4);
                break;
            case frame_type::ping:
                value.payload.resize(8);
                break;
            default:
                value.payload.resize(pick(96));
                break;
            }
            for (auto& octet : value.payload) {
                if (value.type != frame_type::headers || pick(16) == 0) {
                    octet = static_cast<std::byte>(pick(256));
                }
            }
            append(value);
        }
        if (pick(4) == 0 && !bytes.empty()) {
            bytes[pick(static_cast<std::uint32_t>(bytes.size()))] ^= std::byte{0x5a};
        }
        liquid::http2::fuzz::run_input(std::span<const std::uint8_t>(
            reinterpret_cast<const std::uint8_t*>(bytes.data()), bytes.size()));
    }
}

} // namespace
