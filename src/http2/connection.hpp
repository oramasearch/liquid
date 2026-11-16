// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include <boost/capy/io/any_stream.hpp>
#include <boost/capy/task.hpp>
#include <boost/corosio/tls_stream.hpp>

#include "http2/frame.hpp"
#include "http2/hpack.hpp"
#include "liquid/runtime.hpp"

namespace liquid::http2 {

enum class endpoint_role {
    client,
    server,
};

// RFC 9113 section 7 error codes, carried by RST_STREAM and GOAWAY.
enum class reset_code : std::uint32_t {
    no_error = 0x0,
    protocol_error = 0x1,
    internal_error = 0x2,
    flow_control_error = 0x3,
    stream_closed = 0x5,
    frame_size_error = 0x6,
    refused_stream = 0x7,
    cancel = 0x8,
    compression_error = 0x9,
    enhance_your_calm = 0xb,
};

// A token bucket: `burst` events at once, refilled at `per_second`. A peer that spends the bucket
// faster than it refills is flooding, and the connection closes with GOAWAY ENHANCE_YOUR_CALM.
struct flood_limit {
    std::uint32_t burst{};
    std::uint32_t per_second{};
};

// What this endpoint advertises and enforces. Each SETTINGS value here limits the peer; the
// peer's own SETTINGS limit what this endpoint sends.
struct connection_options {
    // SETTINGS_INITIAL_WINDOW_SIZE: the receive window of each stream.
    std::uint32_t initial_window_size{default_window_size};
    // SETTINGS_MAX_FRAME_SIZE: the largest frame payload the peer may send.
    std::uint32_t max_frame_size{default_max_frame_size};
    // SETTINGS_HEADER_TABLE_SIZE: the largest HPACK dynamic table the peer may make this
    // endpoint's decoder hold. It also caps the encoder's table, whatever the peer advertises.
    std::size_t header_table_size{4096};
    // SETTINGS_MAX_CONCURRENT_STREAMS: peer-opened streams past this are refused with
    // RST_STREAM REFUSED_STREAM.
    std::uint32_t max_concurrent_streams{100};
    // SETTINGS_MAX_HEADER_LIST_SIZE: the largest decoded header list, and the largest compressed
    // header block (HEADERS plus CONTINUATION), the peer may send.
    std::uint32_t max_header_list_size{16 * 1024};
    // The most bytes of frames received for streams the caller is not reading yet. Flow control
    // already bounds queued DATA; this is the backstop for everything queued.
    std::size_t max_pending_bytes{16U * 1024U * 1024U};
    // Rapid reset (CVE-2023-44487): streams the peer resets, and streams refused because they were
    // over the concurrent-stream limit.
    flood_limit resets{200, 100};
    // PING frames that need an acknowledgement.
    flood_limit pings{50, 10};
    // SETTINGS frames that need an acknowledgement.
    flood_limit settings{50, 10};
    // Frames that carry no work: empty DATA or CONTINUATION without an end flag, PRIORITY, and
    // frame types this endpoint ignores.
    flood_limit empty_frames{100, 50};
};

[[nodiscard]] auto selected_tls_protocol(const boost::corosio::tls_stream& stream) noexcept
    -> std::string_view;
[[nodiscard]] auto validate_tls_protocol(const boost::corosio::tls_stream& stream) -> result<void>;

class connection {
public:
    connection(boost::capy::any_stream& stream,
               endpoint_role role,
               connection_options options = {});

    [[nodiscard]] auto start() -> runtime::task<void>;
    [[nodiscard]] auto open_stream(std::span<const header_field> headers, bool end_stream)
        -> runtime::task<std::uint32_t>;
    [[nodiscard]] auto send_headers(std::uint32_t stream_id,
                                    std::span<const header_field> headers,
                                    bool end_stream) -> runtime::task<void>;
    [[nodiscard]] auto send_data(std::uint32_t stream_id,
                                 std::vector<std::byte> contents,
                                 bool end_stream) -> runtime::task<void>;
    [[nodiscard]] auto receive_headers(std::uint32_t stream_id)
        -> runtime::task<std::vector<header_field>>;
    [[nodiscard]] auto receive_stream() -> runtime::task<std::uint32_t>;
    [[nodiscard]] auto receive_data(std::uint32_t stream_id)
        -> runtime::task<std::vector<std::byte>>;
    [[nodiscard]] auto cancel(std::uint32_t stream_id) -> runtime::task<void>;
    [[nodiscard]] auto ping(std::span<const std::byte, 8> opaque) -> runtime::task<void>;
    [[nodiscard]] auto goaway() -> runtime::task<void>;
    [[nodiscard]] auto wait_for_goaway() -> runtime::task<void>;

    // Streams with state: open or half-closed, or closed with frames the caller has not read.
    [[nodiscard]] auto tracked_streams() const noexcept -> std::size_t {
        return streams_.size();
    }
    // Bytes queued for streams the caller has not read yet.
    [[nodiscard]] auto pending_bytes() const noexcept -> std::size_t {
        return pending_bytes_;
    }
    // The size the peer's SETTINGS_HEADER_TABLE_SIZE let this endpoint's encoder use.
    [[nodiscard]] auto encoder_table_size() const noexcept -> std::size_t {
        return encoder_.max_table_size();
    }
    // The limit this endpoint's decoder enforces on the peer's dynamic table.
    [[nodiscard]] auto decoder_table_limit() const noexcept -> std::size_t {
        return decoder_.max_table_size();
    }

private:
    struct stream_state {
        std::uint32_t send_window{default_window_size};
        std::uint32_t receive_window{default_window_size};
        bool sent_end_stream{};
        bool received_end_stream{};
        // receive_stream or receive_headers has handed this stream to the caller.
        bool accepted{};
    };

    // One received item for a stream, in wire order: a DATA payload, or a header block that has
    // already been decoded (HPACK state must follow the order blocks arrive in).
    struct inbound {
        std::uint32_t stream_id{};
        frame_type type{};
        bool end_stream{};
        std::vector<std::byte> data;
        std::vector<header_field> headers;
        // Flow-control credit this item consumed; returned to the peer when the caller reads it.
        std::uint32_t credit{};
        std::size_t size{};
    };

    class bucket {
    public:
        explicit bucket(flood_limit limit);
        [[nodiscard]] auto take() -> bool;

    private:
        flood_limit limit_;
        double tokens_;
        std::chrono::steady_clock::time_point last_;
    };

    // What to do with a header block once it is decoded.
    enum class block_target {
        deliver,
        refuse,
        discard,
    };

    boost::capy::any_stream& stream_;
    endpoint_role role_;
    connection_options options_;
    hpack_encoder encoder_;
    hpack_decoder decoder_;
    std::map<std::uint32_t, stream_state> streams_;
    std::map<std::uint32_t, std::deque<inbound>> pending_;
    std::size_t pending_bytes_{};
    std::uint32_t next_stream_id_;
    std::uint32_t last_peer_stream_id_{};
    std::uint32_t peer_max_frame_size_;
    std::uint32_t peer_initial_window_;
    std::uint32_t peer_max_concurrent_streams_{0xffff'ffffU};
    std::uint32_t peer_max_header_list_size_{0xffff'ffffU};
    std::uint32_t connection_send_window_;
    std::uint32_t connection_receive_window_;
    // The header block being reassembled from HEADERS and CONTINUATION frames.
    std::uint32_t continuation_stream_{};
    bool continuation_end_stream_{};
    block_target continuation_target_{block_target::deliver};
    std::vector<std::byte> header_block_;
    bucket resets_;
    bucket pings_;
    bucket settings_;
    bucket empty_frames_;
    bool started_{};
    bool sent_goaway_{};
    bool received_goaway_{};

    [[nodiscard]] auto write_frame(frame value) -> runtime::task<void>;
    [[nodiscard]] auto read_frame() -> runtime::task<frame>;
    // Reads frames until one carries an item for a stream the caller may read; applies every
    // connection-level frame and every limit on the way.
    [[nodiscard]] auto receive_inbound() -> runtime::task<std::optional<inbound>>;
    [[nodiscard]] auto receive_headers_frame(frame value) -> runtime::task<std::optional<inbound>>;
    [[nodiscard]] auto receive_continuation(frame value) -> runtime::task<std::optional<inbound>>;
    [[nodiscard]] auto
    finish_header_block(std::uint32_t stream_id, bool end_stream, block_target target)
        -> runtime::task<std::optional<inbound>>;
    [[nodiscard]] auto receive_data_frame(frame value) -> runtime::task<std::optional<inbound>>;
    [[nodiscard]] auto receive_reset(frame value) -> runtime::task<void>;
    [[nodiscard]] auto receive_window_update(frame value) -> runtime::task<void>;
    [[nodiscard]] auto next_for_stream(std::uint32_t stream_id) -> runtime::task<inbound>;
    [[nodiscard]] auto deliver(inbound value) -> runtime::task<inbound>;
    [[nodiscard]] auto queue(inbound value) -> runtime::task<void>;
    [[nodiscard]] auto await_send_credit(std::uint32_t stream_id) -> runtime::task<void>;
    // Records END_STREAM sent on a stream and retires the stream when it is finished.
    [[nodiscard]] auto finish_sending(std::uint32_t stream_id) -> runtime::task<void>;
    [[nodiscard]] auto apply_control(frame value) -> runtime::task<void>;
    [[nodiscard]] auto send_settings() -> runtime::task<void>;
    [[nodiscard]] auto send_window_update(std::uint32_t stream_id, std::uint32_t increment)
        -> runtime::task<void>;
    [[nodiscard]] auto send_reset(std::uint32_t stream_id, reset_code code) -> runtime::task<void>;
    [[nodiscard]] auto return_connection_credit(std::uint32_t credit) -> runtime::task<void>;
    // Sends GOAWAY with `code` and returns the error that ends the connection.
    [[nodiscard]] auto fail(reset_code code, std::string message) -> boost::capy::task<error>;
    // Forgets a stream: drops what is queued for it and returns that flow-control credit.
    [[nodiscard]] auto retire(std::uint32_t stream_id) -> runtime::task<void>;
    // Retires a stream once both directions have ended and the caller has read everything.
    void forget_if_finished(std::uint32_t stream_id);
    [[nodiscard]] auto active_peer_streams() const -> std::uint32_t;
    [[nodiscard]] auto is_peer_stream(std::uint32_t stream_id) const noexcept -> bool;
    [[nodiscard]] auto is_closed_stream(std::uint32_t stream_id) const noexcept -> bool;
};

} // namespace liquid::http2
