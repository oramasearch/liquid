// SPDX-License-Identifier: MIT

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
constexpr auto max_window = std::uint32_t{0x7fff'ffffU};
constexpr auto max_frame_size_limit = std::uint32_t{16'777'215U};
constexpr auto default_header_table_size = std::size_t{4096};
// Charged against max_pending_bytes for each queued item, so that items with no payload are
// bounded too.
constexpr auto pending_item_overhead = std::size_t{64};

constexpr auto setting_header_table_size = std::uint16_t{0x1};
constexpr auto setting_enable_push = std::uint16_t{0x2};
constexpr auto setting_max_concurrent_streams = std::uint16_t{0x3};
constexpr auto setting_initial_window_size = std::uint16_t{0x4};
constexpr auto setting_max_frame_size = std::uint16_t{0x5};
constexpr auto setting_max_header_list_size = std::uint16_t{0x6};

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

void append_setting(std::vector<std::byte>& payload, std::uint16_t id, std::uint32_t value) {
    const auto setting = setting_payload(id, value);
    payload.insert(payload.end(), setting.begin(), setting.end());
}

auto clamp_to_u32(std::size_t value) -> std::uint32_t {
    return static_cast<std::uint32_t>(
        std::min<std::size_t>(value, std::numeric_limits<std::uint32_t>::max()));
}

// The connection receive window this endpoint grants. Every stream's unread DATA is bounded by its
// stream window, so a connection window of one more stream window than the concurrent-stream limit
// can never be exhausted by streams the caller is not reading yet. That keeps a caller that reads
// one stream at a time from deadlocking behind the others.
auto connection_window_target(const connection_options& options) -> std::uint32_t {
    const auto wanted = (static_cast<std::uint64_t>(options.max_concurrent_streams) + 1U) *
                        static_cast<std::uint64_t>(options.initial_window_size);
    const auto bounded = std::min<std::uint64_t>(
        {wanted, max_window, static_cast<std::uint64_t>(options.max_pending_bytes)});
    return static_cast<std::uint32_t>(std::max<std::uint64_t>(bounded, default_window_size));
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

connection::bucket::bucket(flood_limit limit)
    : limit_(limit), tokens_(static_cast<double>(limit.burst)),
      last_(std::chrono::steady_clock::now()) {}

auto connection::bucket::take() -> bool {
    const auto now = std::chrono::steady_clock::now();
    const auto elapsed = std::chrono::duration<double>(now - last_).count();
    last_ = now;
    tokens_ = std::min(static_cast<double>(limit_.burst),
                       tokens_ + elapsed * static_cast<double>(limit_.per_second));
    if (tokens_ < 1.0) {
        return false;
    }
    tokens_ -= 1.0;
    return true;
}

// Until the peer acknowledges this endpoint's SETTINGS it may still assume the protocol defaults,
// so the decoder starts at the larger of the default and the configured table size.
connection::connection(boost::capy::any_stream& stream,
                       endpoint_role role,
                       connection_options options)
    : stream_(stream), role_(role), options_(options),
      encoder_(std::min(options.header_table_size, default_header_table_size)),
      decoder_(std::max(options.header_table_size, default_header_table_size)),
      next_stream_id_(role == endpoint_role::client ? 1U : 2U),
      peer_max_frame_size_(default_max_frame_size), peer_initial_window_(default_window_size),
      connection_send_window_(default_window_size), connection_receive_window_(default_window_size),
      resets_(options.resets), pings_(options.pings), settings_(options.settings),
      empty_frames_(options.empty_frames) {}

auto connection::fail(reset_code code, std::string message) -> boost::capy::task<error> {
    if (!sent_goaway_) {
        sent_goaway_ = true;
        auto payload = std::vector<std::byte>{};
        const auto last = integer_payload(last_peer_stream_id_);
        const auto value = static_cast<std::uint32_t>(code);
        payload.insert(payload.end(), last.begin(), last.end());
        payload.push_back(static_cast<std::byte>((value >> 24) & 0xffU));
        payload.push_back(static_cast<std::byte>((value >> 16) & 0xffU));
        payload.push_back(static_cast<std::byte>((value >> 8) & 0xffU));
        payload.push_back(static_cast<std::byte>(value & 0xffU));
        // The connection is ending either way; a failed GOAWAY write changes nothing.
        static_cast<void>(
            co_await write_frame(frame{frame_type::goaway, 0, 0, std::move(payload)}));
    }
    co_return error{code == reset_code::enhance_your_calm ? errc::busy : errc::corrupt,
                    std::move(message),
                    "HTTP/2"};
}

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
    // The limit on what the peer sends is the SETTINGS_MAX_FRAME_SIZE this endpoint advertised.
    if (length > options_.max_frame_size) {
        co_return std::unexpected(co_await fail(reset_code::frame_size_error,
                                                "peer frame exceeds SETTINGS_MAX_FRAME_SIZE"));
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
    append_setting(payload, setting_header_table_size, clamp_to_u32(options_.header_table_size));
    if (role_ == endpoint_role::client) {
        append_setting(payload, setting_enable_push, 0);
    }
    append_setting(payload, setting_max_concurrent_streams, options_.max_concurrent_streams);
    append_setting(payload, setting_initial_window_size, options_.initial_window_size);
    append_setting(payload, setting_max_frame_size, options_.max_frame_size);
    append_setting(payload, setting_max_header_list_size, options_.max_header_list_size);
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
    if (options_.max_frame_size < default_max_frame_size ||
        options_.max_frame_size > max_frame_size_limit ||
        options_.initial_window_size > max_window ||
        options_.header_table_size > std::numeric_limits<std::uint32_t>::max()) {
        co_return std::unexpected(
            error{errc::invalid_argument, "HTTP/2 connection options are invalid", "HTTP/2"});
    }
    if (role_ == endpoint_role::client) {
        const auto [write_error, written] = co_await boost::capy::write(
            stream_,
            boost::capy::const_buffer(client_preface,
                                      std::char_traits<char>::length(client_preface)));
        if (write_error || written != std::char_traits<char>::length(client_preface)) {
            co_return std::unexpected(io_error(write_error, "HTTP/2 write client preface"));
        }
    } else {
        auto preface = std::array<char, 24>{};
        const auto [read_error, read] = co_await boost::capy::read(
            stream_, boost::capy::mutable_buffer(preface.data(), preface.size()));
        if (read_error || read != preface.size() ||
            std::string_view(preface.data(), preface.size()) != client_preface) {
            co_return std::unexpected(protocol_error("HTTP/2 client preface is invalid"));
        }
    }
    auto settings = co_await send_settings();
    if (!settings) {
        co_return std::unexpected(settings.error());
    }
    const auto target = connection_window_target(options_);
    if (target > connection_receive_window_) {
        auto raised = co_await send_window_update(0, target - connection_receive_window_);
        if (!raised) {
            co_return std::unexpected(raised.error());
        }
        connection_receive_window_ = target;
    }
    auto received_peer_settings = false;
    while (!received_peer_settings) {
        auto received = co_await read_frame();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (received->type != frame_type::settings) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "first peer frame is not SETTINGS"));
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
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "SETTINGS frame is malformed"));
        }
        if ((value.flags & flag_ack) != 0) {
            if (!empty_frames_.take()) {
                co_return std::unexpected(co_await fail(reset_code::enhance_your_calm,
                                                        "peer sends SETTINGS ACK too often"));
            }
            // The peer now applies this endpoint's SETTINGS_HEADER_TABLE_SIZE.
            decoder_.set_max_table_size(options_.header_table_size);
            co_return {};
        }
        if (!settings_.take()) {
            co_return std::unexpected(co_await fail(reset_code::enhance_your_calm,
                                                    "peer sends SETTINGS too often (flood)"));
        }
        for (std::size_t offset = 0; offset < value.payload.size(); offset += 6) {
            const auto id =
                (byte_value(value.payload[offset]) << 8) | byte_value(value.payload[offset + 1]);
            const auto setting = (byte_value(value.payload[offset + 2]) << 24) |
                                 (byte_value(value.payload[offset + 3]) << 16) |
                                 (byte_value(value.payload[offset + 4]) << 8) |
                                 byte_value(value.payload[offset + 5]);
            if (id == setting_header_table_size) {
                // The peer's value limits this endpoint's encoder, never its decoder.
                const auto size = std::min<std::size_t>(setting, options_.header_table_size);
                if (size != encoder_.max_table_size()) {
                    encoder_.set_max_table_size(size);
                }
            } else if (id == setting_enable_push) {
                if (setting > 1 || (role_ == endpoint_role::client && setting != 0)) {
                    co_return std::unexpected(co_await fail(reset_code::protocol_error,
                                                            "SETTINGS_ENABLE_PUSH is invalid"));
                }
            } else if (id == setting_max_concurrent_streams) {
                peer_max_concurrent_streams_ = setting;
            } else if (id == setting_initial_window_size) {
                if (setting > max_window) {
                    co_return std::unexpected(co_await fail(
                        reset_code::flow_control_error, "SETTINGS_INITIAL_WINDOW_SIZE is invalid"));
                }
                const auto delta = static_cast<std::int64_t>(setting) - peer_initial_window_;
                for (auto& [stream_id, state] : streams_) {
                    const auto updated = static_cast<std::int64_t>(state.send_window) + delta;
                    if (updated < 0 || updated > static_cast<std::int64_t>(max_window)) {
                        co_return std::unexpected(co_await fail(reset_code::flow_control_error,
                                                                "stream send window is invalid"));
                    }
                    state.send_window = static_cast<std::uint32_t>(updated);
                }
                peer_initial_window_ = setting;
            } else if (id == setting_max_frame_size) {
                if (setting < default_max_frame_size || setting > max_frame_size_limit) {
                    co_return std::unexpected(co_await fail(reset_code::protocol_error,
                                                            "SETTINGS_MAX_FRAME_SIZE is invalid"));
                }
                peer_max_frame_size_ = setting;
            } else if (id == setting_max_header_list_size) {
                peer_max_header_list_size_ = setting;
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
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "PING frame is malformed"));
        }
        if ((value.flags & flag_ack) != 0) {
            if (!empty_frames_.take()) {
                co_return std::unexpected(
                    co_await fail(reset_code::enhance_your_calm, "peer sends PING ACK too often"));
            }
            co_return {};
        }
        if (!pings_.take()) {
            co_return std::unexpected(
                co_await fail(reset_code::enhance_your_calm, "peer sends PING too often (flood)"));
        }
        auto acknowledged =
            co_await write_frame(frame{frame_type::ping, flag_ack, 0, std::move(value.payload)});
        if (!acknowledged) {
            co_return std::unexpected(acknowledged.error());
        }
        co_return {};
    }
    if (value.type == frame_type::window_update) {
        if (value.payload.size() != 4) {
            co_return std::unexpected(
                co_await fail(reset_code::frame_size_error, "WINDOW_UPDATE frame is malformed"));
        }
        if (value.stream_id != 0) {
            co_return co_await receive_window_update(std::move(value));
        }
        const auto increment = parse_integer(value.payload);
        if (increment == 0) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "WINDOW_UPDATE increment is zero"));
        }
        if (increment > max_window - connection_send_window_) {
            co_return std::unexpected(
                co_await fail(reset_code::flow_control_error, "flow-control window overflows"));
        }
        connection_send_window_ += increment;
        co_return {};
    }
    if (value.type == frame_type::goaway) {
        if (value.stream_id != 0 || value.payload.size() < 8) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "GOAWAY frame is malformed"));
        }
        received_goaway_ = true;
        co_return {};
    }
    co_return {};
}

auto connection::is_peer_stream(std::uint32_t stream_id) const noexcept -> bool {
    const auto odd = (stream_id & 1U) != 0;
    return role_ == endpoint_role::server ? odd : !odd;
}

auto connection::is_closed_stream(std::uint32_t stream_id) const noexcept -> bool {
    if (stream_id == 0 || streams_.contains(stream_id)) {
        return false;
    }
    if (is_peer_stream(stream_id)) {
        return stream_id <= last_peer_stream_id_;
    }
    return stream_id < next_stream_id_;
}

auto connection::active_peer_streams() const -> std::uint32_t {
    auto count = std::uint32_t{};
    for (const auto& [stream_id, state] : streams_) {
        if (is_peer_stream(stream_id)) {
            ++count;
        }
    }
    return count;
}

auto connection::send_reset(std::uint32_t stream_id, reset_code code) -> runtime::task<void> {
    const auto payload = integer_payload(static_cast<std::uint32_t>(code));
    co_return co_await write_frame(
        frame{frame_type::rst_stream, 0, stream_id, {payload.begin(), payload.end()}});
}

auto connection::return_connection_credit(std::uint32_t credit) -> runtime::task<void> {
    if (credit == 0) {
        co_return {};
    }
    connection_receive_window_ += credit;
    co_return co_await send_window_update(0, credit);
}

auto connection::retire(std::uint32_t stream_id) -> runtime::task<void> {
    auto credit = std::uint32_t{};
    if (const auto found = pending_.find(stream_id); found != pending_.end()) {
        for (const auto& item : found->second) {
            pending_bytes_ -= item.size;
            credit += item.credit;
        }
        pending_.erase(found);
    }
    streams_.erase(stream_id);
    co_return co_await return_connection_credit(credit);
}

void connection::forget_if_finished(std::uint32_t stream_id) {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end() || !found->second.sent_end_stream ||
        !found->second.received_end_stream || pending_.contains(stream_id)) {
        return;
    }
    streams_.erase(found);
}

auto connection::queue(inbound value) -> runtime::task<void> {
    if (value.size >
        options_.max_pending_bytes - std::min(pending_bytes_, options_.max_pending_bytes)) {
        co_return std::unexpected(co_await fail(reset_code::enhance_your_calm,
                                                "frames queued for unread streams exceed the "
                                                "connection's buffer limit"));
    }
    pending_bytes_ += value.size;
    pending_[value.stream_id].push_back(std::move(value));
    co_return {};
}

auto connection::deliver(inbound value) -> runtime::task<inbound> {
    // Flow-control credit goes back to the peer only as the caller reads, so unread DATA stays
    // bounded by the windows this endpoint advertised.
    if (value.credit != 0) {
        auto returned = co_await return_connection_credit(value.credit);
        if (!returned) {
            co_return std::unexpected(returned.error());
        }
        if (const auto found = streams_.find(value.stream_id);
            found != streams_.end() && !found->second.received_end_stream) {
            found->second.receive_window += value.credit;
            auto updated = co_await send_window_update(value.stream_id, value.credit);
            if (!updated) {
                co_return std::unexpected(updated.error());
            }
        }
    }
    if (value.end_stream) {
        forget_if_finished(value.stream_id);
    }
    co_return std::move(value);
}

auto connection::receive_window_update(frame value) -> runtime::task<void> {
    const auto found = streams_.find(value.stream_id);
    if (found == streams_.end()) {
        if (is_closed_stream(value.stream_id)) {
            co_return {};
        }
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "WINDOW_UPDATE on an idle stream"));
    }
    const auto increment = parse_integer(value.payload);
    if (increment == 0) {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "WINDOW_UPDATE increment is zero"));
    }
    if (increment > max_window - found->second.send_window) {
        co_return std::unexpected(
            co_await fail(reset_code::flow_control_error, "flow-control window overflows"));
    }
    found->second.send_window += increment;
    co_return {};
}

auto connection::receive_reset(frame value) -> runtime::task<void> {
    if (value.stream_id == 0) {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "RST_STREAM frame is malformed"));
    }
    if (value.payload.size() != 4) {
        co_return std::unexpected(
            co_await fail(reset_code::frame_size_error, "RST_STREAM frame is malformed"));
    }
    if (!streams_.contains(value.stream_id)) {
        if (!is_closed_stream(value.stream_id)) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "RST_STREAM on an idle stream"));
        }
        if (!empty_frames_.take()) {
            co_return std::unexpected(co_await fail(reset_code::enhance_your_calm,
                                                    "peer resets closed streams too often"));
        }
        co_return {};
    }
    if (!resets_.take()) {
        co_return std::unexpected(co_await fail(reset_code::enhance_your_calm,
                                                "peer resets streams too fast (rapid reset)"));
    }
    co_return co_await retire(value.stream_id);
}

auto connection::receive_headers_frame(frame value) -> runtime::task<std::optional<inbound>> {
    const auto stream_id = value.stream_id;
    if (stream_id == 0) {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "HEADERS frame has stream ID zero"));
    }
    auto offset = std::size_t{};
    auto padding = std::size_t{};
    if ((value.flags & flag_padded) != 0) {
        if (value.payload.empty()) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "padded HEADERS has no pad length"));
        }
        padding = byte_value(value.payload.front());
        offset = 1;
    }
    if ((value.flags & flag_priority) != 0) {
        constexpr auto priority_size = std::size_t{5};
        if (value.payload.size() < offset + priority_size) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "HEADERS priority is truncated"));
        }
        offset += priority_size;
    }
    if (value.payload.size() < offset + padding) {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "HEADERS padding exceeds payload"));
    }

    auto target = block_target::deliver;
    if (const auto found = streams_.find(stream_id); found != streams_.end()) {
        if (found->second.received_end_stream) {
            co_return std::unexpected(
                co_await fail(reset_code::stream_closed, "HEADERS after END_STREAM"));
        }
    } else if (is_peer_stream(stream_id) && stream_id > last_peer_stream_id_) {
        if (role_ == endpoint_role::client) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "server push is not supported"));
        }
        last_peer_stream_id_ = stream_id;
        if (sent_goaway_) {
            target = block_target::discard;
        } else if (active_peer_streams() >= options_.max_concurrent_streams) {
            target = block_target::refuse;
        } else {
            auto state = stream_state{};
            state.send_window = peer_initial_window_;
            state.receive_window = options_.initial_window_size;
            streams_.emplace(stream_id, state);
        }
    } else if (is_closed_stream(stream_id)) {
        target = block_target::discard;
    } else {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "HEADERS on an idle stream"));
    }

    const auto fragment_size = value.payload.size() - offset - padding;
    if (fragment_size > options_.max_header_list_size) {
        co_return std::unexpected(co_await fail(
            reset_code::enhance_your_calm, "header block exceeds SETTINGS_MAX_HEADER_LIST_SIZE"));
    }
    header_block_.assign(value.payload.begin() + static_cast<std::ptrdiff_t>(offset),
                         value.payload.end() - static_cast<std::ptrdiff_t>(padding));
    const auto end_stream = (value.flags & flag_end_stream) != 0;
    if ((value.flags & flag_end_headers) != 0) {
        co_return co_await finish_header_block(stream_id, end_stream, target);
    }
    continuation_stream_ = stream_id;
    continuation_end_stream_ = end_stream;
    continuation_target_ = target;
    co_return std::nullopt;
}

auto connection::receive_continuation(frame value) -> runtime::task<std::optional<inbound>> {
    if (continuation_stream_ == 0) {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "CONTINUATION does not follow HEADERS"));
    }
    const auto end_headers = (value.flags & flag_end_headers) != 0;
    if (value.payload.empty() && !end_headers && !empty_frames_.take()) {
        co_return std::unexpected(
            co_await fail(reset_code::enhance_your_calm, "empty CONTINUATION flood"));
    }
    if (value.payload.size() > options_.max_header_list_size - header_block_.size()) {
        co_return std::unexpected(co_await fail(
            reset_code::enhance_your_calm,
            "header block exceeds SETTINGS_MAX_HEADER_LIST_SIZE (CONTINUATION flood)"));
    }
    header_block_.insert(header_block_.end(), value.payload.begin(), value.payload.end());
    if (!end_headers) {
        co_return std::nullopt;
    }
    const auto stream_id = continuation_stream_;
    continuation_stream_ = 0;
    co_return co_await finish_header_block(
        stream_id, continuation_end_stream_, continuation_target_);
}

auto connection::finish_header_block(std::uint32_t stream_id, bool end_stream, block_target target)
    -> runtime::task<std::optional<inbound>> {
    // Every block is decoded in the order it arrives, refused and discarded ones included, so the
    // HPACK dynamic table stays the same on both ends.
    auto decoded = decoder_.decode(header_block_, options_.max_header_list_size);
    header_block_.clear();
    if (!decoded) {
        const auto too_large =
            decoded.error().message.find("SETTINGS_MAX_HEADER_LIST_SIZE") != std::string::npos;
        co_return std::unexpected(
            co_await fail(too_large ? reset_code::enhance_your_calm : reset_code::compression_error,
                          decoded.error().message));
    }
    if (target == block_target::refuse) {
        if (!resets_.take()) {
            co_return std::unexpected(
                co_await fail(reset_code::enhance_your_calm,
                              "peer opens streams past SETTINGS_MAX_CONCURRENT_STREAMS too fast"));
        }
        auto refused = co_await send_reset(stream_id, reset_code::refused_stream);
        if (!refused) {
            co_return std::unexpected(refused.error());
        }
        co_return std::nullopt;
    }
    if (target == block_target::discard) {
        co_return std::nullopt;
    }
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        co_return std::nullopt;
    }
    if (end_stream) {
        found->second.received_end_stream = true;
    }
    auto output = inbound{};
    output.stream_id = stream_id;
    output.type = frame_type::headers;
    output.end_stream = end_stream;
    output.size = header_list_size(*decoded) + pending_item_overhead;
    output.headers = std::move(*decoded);
    co_return output;
}

auto connection::receive_data_frame(frame value) -> runtime::task<std::optional<inbound>> {
    const auto stream_id = value.stream_id;
    if (stream_id == 0) {
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "DATA frame has stream ID zero"));
    }
    // The whole payload, padding included, counts against flow control.
    const auto size = static_cast<std::uint32_t>(value.payload.size());
    if (size > connection_receive_window_) {
        co_return std::unexpected(
            co_await fail(reset_code::flow_control_error, "connection receive window is exceeded"));
    }
    connection_receive_window_ -= size;
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        if (!is_closed_stream(stream_id)) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "DATA on an idle stream"));
        }
        auto returned = co_await return_connection_credit(size);
        if (!returned) {
            co_return std::unexpected(returned.error());
        }
        co_return std::nullopt;
    }
    auto& state = found->second;
    if (state.received_end_stream) {
        co_return std::unexpected(
            co_await fail(reset_code::stream_closed, "DATA after END_STREAM"));
    }
    if (size > state.receive_window) {
        co_return std::unexpected(
            co_await fail(reset_code::flow_control_error, "stream receive window is exceeded"));
    }
    state.receive_window -= size;
    auto begin = std::size_t{};
    auto end = value.payload.size();
    if ((value.flags & flag_padded) != 0) {
        if (value.payload.empty() || byte_value(value.payload.front()) >= value.payload.size()) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "DATA padding exceeds payload"));
        }
        begin = 1;
        end -= byte_value(value.payload.front());
    }
    const auto end_stream = (value.flags & flag_end_stream) != 0;
    if (begin == end && !end_stream && !empty_frames_.take()) {
        co_return std::unexpected(
            co_await fail(reset_code::enhance_your_calm, "empty DATA frame flood"));
    }
    if (end_stream) {
        state.received_end_stream = true;
    }
    auto output = inbound{};
    output.stream_id = stream_id;
    output.type = frame_type::data;
    output.end_stream = end_stream;
    output.credit = size;
    output.data.assign(value.payload.begin() + static_cast<std::ptrdiff_t>(begin),
                       value.payload.begin() + static_cast<std::ptrdiff_t>(end));
    output.size = output.data.size() + pending_item_overhead;
    co_return output;
}

auto connection::receive_inbound() -> runtime::task<std::optional<inbound>> {
    auto received = co_await read_frame();
    if (!received) {
        co_return std::unexpected(received.error());
    }
    auto value = std::move(*received);
    if (continuation_stream_ != 0 &&
        (value.type != frame_type::continuation || value.stream_id != continuation_stream_)) {
        co_return std::unexpected(co_await fail(reset_code::protocol_error,
                                                "header block is interrupted before END_HEADERS"));
    }
    switch (value.type) {
    case frame_type::data:
        co_return co_await receive_data_frame(std::move(value));
    case frame_type::headers:
        co_return co_await receive_headers_frame(std::move(value));
    case frame_type::continuation:
        co_return co_await receive_continuation(std::move(value));
    case frame_type::rst_stream: {
        auto reset = co_await receive_reset(std::move(value));
        if (!reset) {
            co_return std::unexpected(reset.error());
        }
        co_return std::nullopt;
    }
    case frame_type::settings:
    case frame_type::ping:
    case frame_type::goaway:
    case frame_type::window_update: {
        auto applied = co_await apply_control(std::move(value));
        if (!applied) {
            co_return std::unexpected(applied.error());
        }
        co_return std::nullopt;
    }
    case frame_type::push_promise:
        co_return std::unexpected(
            co_await fail(reset_code::protocol_error, "server push is not supported"));
    case frame_type::priority:
        if (value.stream_id == 0 || value.payload.size() != 5) {
            co_return std::unexpected(
                co_await fail(reset_code::protocol_error, "PRIORITY frame is malformed"));
        }
        break;
    default:
        break;
    }
    // PRIORITY and unknown frame types are ignored, within a budget.
    if (!empty_frames_.take()) {
        co_return std::unexpected(co_await fail(reset_code::enhance_your_calm,
                                                "peer sends frames that carry no work too often"));
    }
    co_return std::nullopt;
}

auto connection::next_for_stream(std::uint32_t stream_id) -> runtime::task<inbound> {
    for (;;) {
        if (const auto found = pending_.find(stream_id);
            found != pending_.end() && !found->second.empty()) {
            auto output = std::move(found->second.front());
            found->second.pop_front();
            pending_bytes_ -= output.size;
            if (found->second.empty()) {
                pending_.erase(found);
            }
            co_return co_await deliver(std::move(output));
        }
        if (is_closed_stream(stream_id)) {
            co_return std::unexpected(protocol_error("stream ended before its expected frame"));
        }
        auto received = co_await receive_inbound();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (!*received) {
            continue;
        }
        if ((*received)->stream_id == stream_id) {
            co_return co_await deliver(std::move(**received));
        }
        auto queued = co_await queue(std::move(**received));
        if (!queued) {
            co_return std::unexpected(queued.error());
        }
    }
}

auto connection::await_send_credit(std::uint32_t stream_id) -> runtime::task<void> {
    for (;;) {
        const auto found = streams_.find(stream_id);
        if (found == streams_.end() ||
            (connection_send_window_ != 0 && found->second.send_window != 0)) {
            co_return {};
        }
        auto received = co_await receive_inbound();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (*received) {
            auto queued = co_await queue(std::move(**received));
            if (!queued) {
                co_return std::unexpected(queued.error());
            }
        }
    }
}

auto connection::finish_sending(std::uint32_t stream_id) -> runtime::task<void> {
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        co_return {};
    }
    found->second.sent_end_stream = true;
    if (role_ == endpoint_role::client) {
        forget_if_finished(stream_id);
        co_return {};
    }
    // A server whose response is complete has no use for the rest of the request: it resets a
    // request still being sent (RFC 9113 section 8.1) and drops what it did not read.
    if (!found->second.received_end_stream) {
        auto reset = co_await send_reset(stream_id, reset_code::no_error);
        if (!reset) {
            co_return std::unexpected(reset.error());
        }
    }
    co_return co_await retire(stream_id);
}

auto connection::send_headers(std::uint32_t stream_id,
                              std::span<const header_field> headers,
                              bool end_stream) -> runtime::task<void> {
    if (!started_ || stream_id == 0 || sent_goaway_) {
        co_return std::unexpected(
            protocol_error("cannot send headers in the current connection state"));
    }
    const auto found = streams_.find(stream_id);
    if (found == streams_.end()) {
        // The peer reset the stream; there is nobody left to send to.
        if (is_closed_stream(stream_id)) {
            co_return {};
        }
        co_return std::unexpected(protocol_error("cannot send headers on an idle stream"));
    }
    if (found->second.sent_end_stream) {
        co_return std::unexpected(protocol_error("cannot send headers after END_STREAM"));
    }
    if (header_list_size(headers) > peer_max_header_list_size_) {
        co_return std::unexpected(error{errc::invalid_argument,
                                        "header list exceeds the peer's "
                                        "SETTINGS_MAX_HEADER_LIST_SIZE",
                                        "HTTP/2"});
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
        if (offset == 0 && end_stream) {
            flags |= flag_end_stream;
        }
        if (offset + count == encoded->size()) {
            flags |= flag_end_headers;
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
    if (end_stream) {
        co_return co_await finish_sending(stream_id);
    }
    co_return {};
}

auto connection::open_stream(std::span<const header_field> headers, bool end_stream)
    -> runtime::task<std::uint32_t> {
    if (received_goaway_) {
        co_return std::unexpected(protocol_error("peer has sent GOAWAY"));
    }
    if (!started_) {
        co_return std::unexpected(
            protocol_error("cannot open a stream before the connection starts"));
    }
    auto own_streams = std::uint32_t{};
    for (const auto& [stream_id, state] : streams_) {
        if (!is_peer_stream(stream_id)) {
            ++own_streams;
        }
    }
    if (own_streams >= peer_max_concurrent_streams_) {
        co_return std::unexpected(
            error{errc::busy, "peer's SETTINGS_MAX_CONCURRENT_STREAMS is reached", "HTTP/2"});
    }
    if (next_stream_id_ > max_window) {
        co_return std::unexpected(protocol_error("stream identifiers are exhausted"));
    }
    const auto stream_id = next_stream_id_;
    next_stream_id_ += 2;
    auto state = stream_state{};
    state.send_window = peer_initial_window_;
    state.receive_window = options_.initial_window_size;
    streams_.emplace(stream_id, state);
    auto sent = co_await send_headers(stream_id, headers, end_stream);
    if (!sent) {
        co_return std::unexpected(sent.error());
    }
    co_return stream_id;
}

auto connection::send_window_update(std::uint32_t stream_id, std::uint32_t increment)
    -> runtime::task<void> {
    if (increment == 0 || increment > max_window) {
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
    if (!started_) {
        co_return std::unexpected(protocol_error("cannot send DATA in the current stream state"));
    }
    if (const auto found = streams_.find(stream_id); found == streams_.end()) {
        // The peer reset the stream; there is nobody left to send to.
        if (is_closed_stream(stream_id)) {
            co_return {};
        }
        co_return std::unexpected(protocol_error("cannot send DATA in the current stream state"));
    } else if (found->second.sent_end_stream) {
        co_return std::unexpected(protocol_error("cannot send DATA in the current stream state"));
    }
    auto offset = std::size_t{};
    do {
        auto count = std::size_t{};
        if (offset < contents.size()) {
            auto credited = co_await await_send_credit(stream_id);
            if (!credited) {
                co_return std::unexpected(credited.error());
            }
            const auto found = streams_.find(stream_id);
            if (found == streams_.end()) {
                co_return {};
            }
            const auto available = std::min(connection_send_window_, found->second.send_window);
            count =
                std::min<std::size_t>({peer_max_frame_size_, available, contents.size() - offset});
        } else if (!streams_.contains(stream_id)) {
            co_return {};
        }
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
        streams_.at(stream_id).send_window -= static_cast<std::uint32_t>(count);
        offset += count;
    } while (offset < contents.size());
    if (end_stream) {
        co_return co_await finish_sending(stream_id);
    }
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
    if (const auto found = streams_.find(stream_id); found != streams_.end()) {
        found->second.accepted = true;
    }
    co_return std::move(first->headers);
}

auto connection::receive_stream() -> runtime::task<std::uint32_t> {
    if (!started_ || role_ != endpoint_role::server || received_goaway_) {
        co_return std::unexpected(
            protocol_error("cannot accept a stream in the current connection state"));
    }
    for (const auto& [stream_id, items] : pending_) {
        const auto found = streams_.find(stream_id);
        if (!items.empty() && items.front().type == frame_type::headers &&
            found != streams_.end() && !found->second.accepted) {
            found->second.accepted = true;
            co_return stream_id;
        }
    }
    for (;;) {
        auto received = co_await receive_inbound();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (!*received) {
            if (received_goaway_) {
                co_return std::unexpected(protocol_error("peer has sent GOAWAY"));
            }
            continue;
        }
        const auto stream_id = (*received)->stream_id;
        const auto is_new = (*received)->type == frame_type::headers && is_peer_stream(stream_id) &&
                            !streams_.at(stream_id).accepted;
        auto queued = co_await queue(std::move(**received));
        if (!queued) {
            co_return std::unexpected(queued.error());
        }
        if (is_new) {
            streams_.at(stream_id).accepted = true;
            co_return stream_id;
        }
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
                // Trailers: leave them for receive_headers.
                pending_bytes_ += received->size;
                pending_[stream_id].push_front(std::move(*received));
                break;
            }
            co_return std::unexpected(protocol_error("expected DATA frame"));
        }
        output.insert(output.end(), received->data.begin(), received->data.end());
        if (received->end_stream) {
            break;
        }
    }
    co_return output;
}

auto connection::receive_data_part(std::uint32_t stream_id) -> runtime::task<data_part> {
    const auto known = streams_.find(stream_id);
    if (known != streams_.end() && known->second.received_end_stream &&
        !pending_.contains(stream_id)) {
        co_return data_part{{}, true};
    }
    auto received = co_await next_for_stream(stream_id);
    if (!received) {
        co_return std::unexpected(received.error());
    }
    if (received->type != frame_type::data) {
        co_return std::unexpected(protocol_error("expected DATA frame"));
    }
    co_return data_part{std::move(received->data), received->end_stream};
}

auto connection::cancel(std::uint32_t stream_id) -> runtime::task<void> {
    if (stream_id == 0) {
        co_return std::unexpected(protocol_error("cannot cancel connection stream zero"));
    }
    auto written = co_await send_reset(stream_id, reset_code::cancel);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return co_await retire(stream_id);
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
    auto payload = std::vector<std::byte>(8);
    const auto last = integer_payload(last_peer_stream_id_);
    std::copy(last.begin(), last.end(), payload.begin());
    auto written = co_await write_frame(frame{frame_type::goaway, 0, 0, std::move(payload)});
    if (!written) {
        co_return std::unexpected(written.error());
    }
    sent_goaway_ = true;
    co_return {};
}

auto connection::wait_for_goaway() -> runtime::task<void> {
    while (!received_goaway_) {
        auto received = co_await receive_inbound();
        if (!received) {
            co_return std::unexpected(received.error());
        }
        if (*received) {
            co_return std::unexpected(protocol_error("expected connection control frame"));
        }
    }
    co_return {};
}

} // namespace liquid::http2
