#include "http2/connection.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <string>
#include <utility>

#include <boost/capy/buffers.hpp>
#include <boost/capy/error.hpp>
#include <boost/capy/read.hpp>
#include <boost/capy/write.hpp>

namespace liquid::http2 {
namespace {

constexpr auto client_preface = "PRI * HTTP/2.0\r\n\r\nSM\r\n\r\n";
constexpr auto flag_padded = std::uint8_t{0x8};
constexpr auto flag_priority = std::uint8_t{0x20};

auto byte_value(std::byte value) -> std::uint32_t {
    return std::to_integer<std::uint8_t>(value);
}

auto integer_payload(std::uint32_t value) -> std::array<std::byte, 4> {
    return {static_cast<std::byte>((value >> 24) & 0x7fU),
            static_cast<std::byte>((value >> 16) & 0xffU),
            static_cast<std::byte>((value >> 8) & 0xffU),
            static_cast<std::byte>(value & 0xffU)};
}

auto parse_integer(std::span<const std::byte> input) -> std::uint32_t {
    return ((byte_value(input[0]) & 0x7fU) << 24) | (byte_value(input[1]) << 16) |
           (byte_value(input[2]) << 8) | byte_value(input[3]);
}

auto io_error(std::error_code error_code, std::string context) -> error {
    return {errc::io, error_code.message(), std::move(context)};
}

auto protocol_error(std::string message) -> error {
    return {errc::corrupt, std::move(message), "HTTP/2"};
}

} // namespace

auto selected_tls_protocol(const boost::corosio::tls_stream& stream) noexcept -> std::string_view {
    return stream.selected_alpn();
}

auto validate_tls_protocol(const boost::corosio::tls_stream& stream) -> result<void> {
    const auto selected = selected_tls_protocol(stream);
    if (selected == "h2") {
        return {};
    }
    return std::unexpected(
        error{errc::unavailable, "TLS peer did not negotiate HTTP/2", "HTTP/2 TLS ALPN"});
}

connection::connection(boost::capy::any_stream& stream,
                       endpoint_role role,
                       connection_options options)
    : stream_(stream), role_(role), options_(options), encoder_(options.header_table_size),
      decoder_(options.header_table_size), next_stream_id_(role == endpoint_role::client ? 1U : 2U),
      peer_max_frame_size_(default_max_frame_size), peer_initial_window_(default_window_size),
      connection_send_window_(default_window_size),
      connection_receive_window_(options.initial_window_size) {}

auto connection::write_frame(frame value) -> runtime::task<void> {
    auto encoded = encode_frame(value);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    const auto [write_error, written] = co_await boost::capy::write(
        stream_, boost::capy::const_buffer(encoded->data(), encoded->size()));
    if (write_error || written != encoded->size()) {
        co_return std::unexpected(io_error(write_error, "HTTP/2 write frame"));
    }
    co_return {};
}

auto connection::read_frame() -> runtime::task<frame> {
    auto header = std::array<std::byte, 9>{};
    const auto [header_error, header_read] = co_await boost::capy::read(
        stream_, boost::capy::mutable_buffer(header.data(), header.size()));
    if (header_error || header_read != header.size()) {
        co_return std::unexpected(io_error(header_error, "HTTP/2 read frame header"));
    }
    const auto length =
        (byte_value(header[0]) << 16) | (byte_value(header[1]) << 8) | byte_value(header[2]);
    if (length > peer_max_frame_size_) {
        co_return std::unexpected(protocol_error("peer frame exceeds SETTINGS_MAX_FRAME_SIZE"));
    }
    auto raw = std::vector<std::byte>(9 + length);
    std::copy(header.begin(), header.end(), raw.begin());
    if (length != 0) {
        const auto [payload_error, payload_read] = co_await boost::capy::read(
            stream_, boost::capy::mutable_buffer(raw.data() + 9, static_cast<std::size_t>(length)));
        if (payload_error || payload_read != length) {
            co_return std::unexpected(io_error(payload_error, "HTTP/2 read frame payload"));
        }
    }
    auto decoded = decode_frame(raw);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return std::move(*decoded);
}

auto connection::send_settings() -> runtime::task<void> {
    auto payload = std::vector<std::byte>{};
    const auto window = setting_payload(0x4, options_.initial_window_size);
    const auto frame_size = setting_payload(0x5, options_.max_frame_size);
    payload.insert(payload.end(), window.begin(), window.end());
    payload.insert(payload.end(), frame_size.begin(), frame_size.end());
    auto written = co_await write_frame(frame{frame_type::settings, 0, 0, std::move(payload)});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return {};
}

auto connection::start() -> runtime::task<void> {
    if (started_) {
        co_return {};
    }
    if (role_ == endpoint_role::client) {
        const auto [write_error, written] = co_await boost::capy::write(
            stream_,
            boost::capy::const_buffer(client_preface,
                                      std::char_traits<char>::length(client_preface)));
        if (write_error || written != std::char_traits<char>::length(client_preface)) {
            co_return std::unexpected(io_error(write_error, "HTTP/2 write client preface"));
        }
        auto settings = co_await send_settings();
        if (!settings) {
            co_return std::unexpected(settings.error());
        }
    } else {
        auto preface = std::array<char, 24>{};
        const auto [read_error, read] = co_await boost::capy::read(
            stream_, boost::capy::mutable_buffer(preface.data(), preface.size()));
        if (read_error || read != preface.size() ||
            std::string_view(preface.data(), preface.size()) != client_preface) {
            co_return std::unexpected(protocol_error("HTTP/2 client preface is invalid"));
        }
        auto settings = co_await send_settings();
        if (!settings) {
            co_return std::unexpected(settings.error());
        }
    }
    auto received_peer_settings = false;
    while (!received_peer_settings) {
        auto received = co_await read_frame();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type != frame_type::settings) {
            co_return std::unexpected(protocol_error("first peer frame is not SETTINGS"));
        }
        const auto peer_acknowledged = (received->flags & flag_ack) != 0;
        auto applied = co_await apply_control(std::move(*received));
        if (!applied) {
            co_return std::unexpected(applied.error());
        }
        received_peer_settings = !peer_acknowledged;
    }
    started_ = true;
    co_return {};
}

auto connection::apply_control(frame value) -> runtime::task<void> {
    if (value.type == frame_type::settings) {
        if (value.stream_id != 0 || ((value.flags & flag_ack) != 0 && !value.payload.empty()) ||
            (value.flags & flag_ack) == 0 && value.payload.size() % 6 != 0) {
            co_return std::unexpected(protocol_error("SETTINGS frame is malformed"));
        }
        if ((value.flags & flag_ack) != 0) {
            co_return {};
        }
        for (std::size_t offset = 0; offset < value.payload.size(); offset += 6) {
            const auto id =
                (byte_value(value.payload[offset]) << 8) | byte_value(value.payload[offset + 1]);
            const auto setting = (byte_value(value.payload[offset + 2]) << 24) |
                                 (byte_value(value.payload[offset + 3]) << 16) |
                                 (byte_value(value.payload[offset + 4]) << 8) |
                                 byte_value(value.payload[offset + 5]);
            if (id == 0x1) {
                decoder_.set_max_table_size(setting);
            } else if (id == 0x4) {
                if (setting > 0x7fff'ffffU) {
                    co_return std::unexpected(
                        protocol_error("SETTINGS_INITIAL_WINDOW_SIZE is invalid"));
                }
                const auto delta = static_cast<std::int64_t>(setting) - peer_initial_window_;
                for (auto& [stream_id, state] : streams_) {
                    const auto updated = static_cast<std::int64_t>(state.send_window) + delta;
                    if (updated < 0 || updated > 0x7fff'ffffLL) {
                        co_return std::unexpected(protocol_error("stream send window is invalid"));
                    }
                    state.send_window = static_cast<std::uint32_t>(updated);
                }
                peer_initial_window_ = setting;
            } else if (id == 0x5) {
                if (setting < default_max_frame_size || setting > 16'777'215U) {
                    co_return std::unexpected(protocol_error("SETTINGS_MAX_FRAME_SIZE is invalid"));
                }
                peer_max_frame_size_ = setting;
            }
        }
        auto acknowledged = co_await write_frame(frame{frame_type::settings, flag_ack, 0, {}});
        if (!acknowledged) {
            co_return std::unexpected(acknowledged.error());
        }
        co_return {};
    }
    if (value.type == frame_type::ping) {
        if (value.stream_id != 0 || value.payload.size() != 8) {
            co_return std::unexpected(protocol_error("PING frame is malformed"));
        }
        if ((value.flags & flag_ack) == 0) {
            auto acknowledged = co_await write_frame(
                frame{frame_type::ping, flag_ack, 0, std::move(value.payload)});
            if (!acknowledged) {
                co_return std::unexpected(acknowledged.error());
            }
        }
        co_return {};
    }
    if (value.type == frame_type::window_update) {
        if (value.payload.size() != 4) {
            co_return std::unexpected(protocol_error("WINDOW_UPDATE frame is malformed"));
        }
        const auto increment = parse_integer(value.payload);
        if (increment == 0) {
            co_return std::unexpected(protocol_error("WINDOW_UPDATE increment is zero"));
        }
        auto* window = &connection_send_window_;
        if (value.stream_id != 0) {
            window = &streams_[value.stream_id].send_window;
        }
        if (increment > 0x7fff'ffffU - *window) {
            co_return std::unexpected(protocol_error("flow-control window overflows"));
        }
        *window += increment;
        co_return {};
    }
    if (value.type == frame_type::rst_stream) {
        if (value.stream_id == 0 || value.payload.size() != 4) {
            co_return std::unexpected(protocol_error("RST_STREAM frame is malformed"));
        }
        streams_[value.stream_id].reset = true;
        co_return {};
    }
    if (value.type == frame_type::goaway) {
        if (value.stream_id != 0 || value.payload.size() < 8) {
            co_return std::unexpected(protocol_error("GOAWAY frame is malformed"));
        }
        received_goaway_ = true;
        co_return {};
    }
    co_return {};
}

auto connection::next_for_stream(std::uint32_t stream_id) -> runtime::task<frame> {
    for (;;) {
        auto& pending = pending_[stream_id];
        if (!pending.empty()) {
            auto output = std::move(pending.front());
            pending.erase(pending.begin());
            if (pending.empty()) {
                pending_.erase(stream_id);
            }
            co_return output;
        }
        auto received = co_await read_frame();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type == frame_type::settings || received->type == frame_type::ping ||
            received->type == frame_type::window_update ||
            received->type == frame_type::rst_stream || received->type == frame_type::goaway) {
            const auto received_stream = received->stream_id;
            const auto control = co_await apply_control(std::move(*received));
            if (!control) {
                co_return std::unexpected(control.error());
            }
            if (received_stream == stream_id && (received_goaway_ || streams_[stream_id].reset)) {
                co_return std::unexpected(protocol_error("stream ended before its expected frame"));
            }
            continue;
        }
        if (received->type == frame_type::push_promise) {
            co_return std::unexpected(protocol_error("server push is not supported"));
        }
        if (received->stream_id == 0) {
            co_return std::unexpected(protocol_error("stream frame has stream ID zero"));
        }
        if (!streams_.contains(received->stream_id)) {
            auto state = stream_state{};
            state.send_window = peer_initial_window_;
            streams_.emplace(received->stream_id, state);
        }
        if (received->type == frame_type::data) {
            const auto size = static_cast<std::uint32_t>(received->payload.size());
            if (size > connection_receive_window_) {
                co_return std::unexpected(protocol_error("connection receive window is exhausted"));
            }
            connection_receive_window_ -= size;
            auto connection_update = co_await send_window_update(0, size);
            if (!connection_update) {
                co_return std::unexpected(connection_update.error());
            }
            auto stream_update = co_await send_window_update(received->stream_id, size);
            if (!stream_update) {
                co_return std::unexpected(stream_update.error());
            }
            connection_receive_window_ += size;
        }
        if ((received->flags & flag_end_stream) != 0) {
            streams_[received->stream_id].received_end_stream = true;
        }
        if (received->stream_id == stream_id) {
            co_return std::move(*received);
        }
        pending_[received->stream_id].push_back(std::move(*received));
    }
}

auto connection::await_send_credit(std::uint32_t stream_id) -> runtime::task<void> {
    while (connection_send_window_ == 0 || streams_[stream_id].send_window == 0) {
        auto received = co_await read_frame();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type == frame_type::settings || received->type == frame_type::ping ||
            received->type == frame_type::window_update ||
            received->type == frame_type::rst_stream || received->type == frame_type::goaway) {
            auto applied = co_await apply_control(std::move(*received));
            if (!applied) {
                co_return std::unexpected(applied.error());
            }
            continue;
        }
        if (received->stream_id == 0) {
            co_return std::unexpected(protocol_error("stream frame has stream ID zero"));
        }
        pending_[received->stream_id].push_back(std::move(*received));
    }
    co_return {};
}

auto connection::send_headers(std::uint32_t stream_id,
                              std::span<const header_field> headers,
                              bool end_stream) -> runtime::task<void> {
    if (!started_ || stream_id == 0 || sent_goaway_) {
        co_return std::unexpected(
            protocol_error("cannot send headers in the current connection state"));
    }
    if (!streams_.contains(stream_id)) {
        auto state = stream_state{};
        state.send_window = peer_initial_window_;
        streams_.emplace(stream_id, state);
    }
    auto encoded = encoder_.encode(headers);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto offset = std::size_t{};
    do {
        const auto count = std::min<std::size_t>(peer_max_frame_size_, encoded->size() - offset);
        auto payload =
            std::vector<std::byte>(encoded->begin() + static_cast<std::ptrdiff_t>(offset),
                                   encoded->begin() + static_cast<std::ptrdiff_t>(offset + count));
        auto flags = std::uint8_t{};
        if (offset + count == encoded->size()) {
            flags |= flag_end_headers;
            if (end_stream) {
                flags |= flag_end_stream;
            }
        }
        auto written =
            co_await write_frame(frame{offset == 0 ? frame_type::headers : frame_type::continuation,
                                       flags,
                                       stream_id,
                                       std::move(payload)});
        if (!written) {
            co_return std::unexpected(written.error());
        }
        offset += count;
    } while (offset < encoded->size());
    streams_[stream_id].sent_end_stream = end_stream;
    co_return {};
}

auto connection::open_stream(std::span<const header_field> headers, bool end_stream)
    -> runtime::task<std::uint32_t> {
    if (received_goaway_) {
        co_return std::unexpected(protocol_error("peer has sent GOAWAY"));
    }
    const auto stream_id = next_stream_id_;
    next_stream_id_ += 2;
    streams_[stream_id].send_window = peer_initial_window_;
    auto sent = co_await send_headers(stream_id, headers, end_stream);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    co_return stream_id;
}

auto connection::send_window_update(std::uint32_t stream_id, std::uint32_t increment)
    -> runtime::task<void> {
    if (increment == 0 || increment > 0x7fff'ffffU) {
        co_return std::unexpected(protocol_error("WINDOW_UPDATE increment is invalid"));
    }
    const auto payload = integer_payload(increment);
    auto written = co_await write_frame(
        frame{frame_type::window_update, 0, stream_id, {payload.begin(), payload.end()}});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return {};
}

auto connection::send_data(std::uint32_t stream_id,
                           std::vector<std::byte> contents,
                           bool end_stream) -> runtime::task<void> {
    if (!started_ || !streams_.contains(stream_id) || streams_[stream_id].sent_end_stream) {
        co_return std::unexpected(protocol_error("cannot send DATA in the current stream state"));
    }
    auto offset = std::size_t{};
    do {
        auto& state = streams_[stream_id];
        while (connection_send_window_ == 0 || state.send_window == 0) {
            auto credited = co_await await_send_credit(stream_id);
            if (!credited) {
                co_return std::unexpected(credited.error());
            }
        }
        const auto available = std::min(connection_send_window_, state.send_window);
        const auto count =
            std::min<std::size_t>({peer_max_frame_size_, available, contents.size() - offset});
        auto payload =
            std::vector<std::byte>(contents.begin() + static_cast<std::ptrdiff_t>(offset),
                                   contents.begin() + static_cast<std::ptrdiff_t>(offset + count));
        auto flags = std::uint8_t{};
        if (offset + count == contents.size() && end_stream) {
            flags |= flag_end_stream;
        }
        auto written =
            co_await write_frame(frame{frame_type::data, flags, stream_id, std::move(payload)});
        if (!written) {
            co_return std::unexpected(written.error());
        }
        connection_send_window_ -= static_cast<std::uint32_t>(count);
        state.send_window -= static_cast<std::uint32_t>(count);
        offset += count;
    } while (offset < contents.size());
    streams_[stream_id].sent_end_stream = end_stream;
    co_return {};
}

auto connection::receive_headers(std::uint32_t stream_id)
    -> runtime::task<std::vector<header_field>> {
    auto first = co_await next_for_stream(stream_id);
    if (!first) {
        co_return std::unexpected(first.error());
    }
    if (first->type != frame_type::headers) {
        co_return std::unexpected(protocol_error("expected HEADERS frame"));
    }
    auto offset = std::size_t{};
    auto padding = std::size_t{};
    if ((first->flags & flag_padded) != 0) {
        if (first->payload.empty()) {
            co_return std::unexpected(protocol_error("padded HEADERS has no pad length"));
        }
        padding = byte_value(first->payload.front());
        offset = 1;
    }
    if ((first->flags & flag_priority) != 0) {
        constexpr auto priority_size = std::size_t{5};
        if (first->payload.size() < offset + priority_size) {
            co_return std::unexpected(protocol_error("HEADERS priority is truncated"));
        }
        offset += priority_size;
    }
    if (first->payload.size() < offset + padding) {
        co_return std::unexpected(protocol_error("HEADERS padding exceeds payload"));
    }
    auto block =
        std::vector<std::byte>(first->payload.begin() + static_cast<std::ptrdiff_t>(offset),
                               first->payload.end() - static_cast<std::ptrdiff_t>(padding));
    while ((first->flags & flag_end_headers) == 0) {
        auto continuation = co_await next_for_stream(stream_id);
        if (!continuation) {
            co_return std::unexpected(continuation.error());
        }
        if (continuation->type != frame_type::continuation) {
            co_return std::unexpected(
                protocol_error("HEADERS block is not followed by CONTINUATION"));
        }
        if ((continuation->flags & (flag_padded | flag_priority)) != 0) {
            co_return std::unexpected(protocol_error("CONTINUATION has HEADERS-only flags"));
        }
        block.insert(block.end(), continuation->payload.begin(), continuation->payload.end());
        first = std::move(continuation);
    }
    auto decoded = decoder_.decode(block);
    if (!decoded) {
        co_return std::unexpected(decoded.error());
    }
    co_return std::move(*decoded);
}

auto connection::receive_stream() -> runtime::task<std::uint32_t> {
    if (!started_ || role_ != endpoint_role::server || received_goaway_) {
        co_return std::unexpected(
            protocol_error("cannot accept a stream in the current connection state"));
    }
    for (const auto& [stream_id, frames] : pending_) {
        if (!frames.empty() && frames.front().type == frame_type::headers) {
            co_return stream_id;
        }
    }
    for (;;) {
        auto received = co_await read_frame();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type == frame_type::settings || received->type == frame_type::ping ||
            received->type == frame_type::window_update ||
            received->type == frame_type::rst_stream || received->type == frame_type::goaway) {
            auto applied = co_await apply_control(std::move(*received));
            if (!applied) {
                co_return std::unexpected(applied.error());
            }
            if (received_goaway_) {
                co_return std::unexpected(protocol_error("peer has sent GOAWAY"));
            }
            continue;
        }
        if (received->type != frame_type::headers || received->stream_id == 0 ||
            (received->stream_id & 1U) == 0) {
            co_return std::unexpected(protocol_error("expected a client HEADERS frame"));
        }
        const auto stream_id = received->stream_id;
        auto state = stream_state{};
        state.send_window = peer_initial_window_;
        streams_.insert_or_assign(stream_id, state);
        pending_[stream_id].push_back(std::move(*received));
        co_return stream_id;
    }
}

auto connection::receive_data(std::uint32_t stream_id) -> runtime::task<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    for (;;) {
        auto received = co_await next_for_stream(stream_id);
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type != frame_type::data) {
            if (received->type == frame_type::headers && !output.empty()) {
                pending_[stream_id].insert(pending_[stream_id].begin(), std::move(*received));
                break;
            }
            co_return std::unexpected(protocol_error("expected DATA frame"));
        }
        output.insert(output.end(), received->payload.begin(), received->payload.end());
        if ((received->flags & flag_end_stream) != 0) {
            break;
        }
    }
    co_return output;
}

auto connection::cancel(std::uint32_t stream_id) -> runtime::task<void> {
    if (stream_id == 0) {
        co_return std::unexpected(protocol_error("cannot cancel connection stream zero"));
    }
    const auto payload = integer_payload(static_cast<std::uint32_t>(reset_code::cancel));
    auto written = co_await write_frame(
        frame{frame_type::rst_stream, 0, stream_id, {payload.begin(), payload.end()}});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    streams_[stream_id].reset = true;
    co_return {};
}

auto connection::ping(std::span<const std::byte, 8> opaque) -> runtime::task<void> {
    auto written =
        co_await write_frame(frame{frame_type::ping, 0, 0, {opaque.begin(), opaque.end()}});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return {};
}

auto connection::goaway() -> runtime::task<void> {
    const auto payload = std::vector<std::byte>(8);
    auto written = co_await write_frame(frame{frame_type::goaway, 0, 0, payload});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    sent_goaway_ = true;
    co_return {};
}

auto connection::wait_for_goaway() -> runtime::task<void> {
    while (!received_goaway_) {
        auto received = co_await read_frame();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type != frame_type::settings && received->type != frame_type::ping &&
            received->type != frame_type::window_update && received->type != frame_type::goaway) {
            co_return std::unexpected(protocol_error("expected connection control frame"));
        }
        auto applied = co_await apply_control(std::move(*received));
        if (!applied) {
            co_return std::unexpected(applied.error());
        }
    }
    co_return {};
}

} // namespace liquid::http2
