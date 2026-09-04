#pragma once

#include <cstddef>
#include <cstdint>
#include <map>
#include <span>
#include <string_view>
#include <vector>

#include <boost/capy/io/any_stream.hpp>
#include <boost/corosio/tls_stream.hpp>

#include "http2/frame.hpp"
#include "http2/hpack.hpp"
#include "liquid/runtime.hpp"

namespace liquid::http2 {

enum class endpoint_role {
    client,
    server,
};

enum class reset_code : std::uint32_t {
    no_error = 0,
    protocol_error = 1,
    internal_error = 2,
    flow_control_error = 3,
    stream_closed = 5,
    cancel = 8,
};

struct connection_options {
    std::uint32_t initial_window_size{default_window_size};
    std::uint32_t max_frame_size{default_max_frame_size};
    std::size_t header_table_size{4096};
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
    [[nodiscard]] auto receive_data(std::uint32_t stream_id)
        -> runtime::task<std::vector<std::byte>>;
    [[nodiscard]] auto cancel(std::uint32_t stream_id) -> runtime::task<void>;
    [[nodiscard]] auto ping(std::span<const std::byte, 8> opaque) -> runtime::task<void>;
    [[nodiscard]] auto goaway() -> runtime::task<void>;
    [[nodiscard]] auto wait_for_goaway() -> runtime::task<void>;

private:
    struct stream_state {
        std::uint32_t send_window{default_window_size};
        bool sent_end_stream{};
        bool received_end_stream{};
        bool reset{};
    };

    boost::capy::any_stream& stream_;
    endpoint_role role_;
    connection_options options_;
    hpack_encoder encoder_;
    hpack_decoder decoder_;
    std::map<std::uint32_t, stream_state> streams_;
    std::map<std::uint32_t, std::vector<frame>> pending_;
    std::uint32_t next_stream_id_;
    std::uint32_t peer_max_frame_size_;
    std::uint32_t peer_initial_window_;
    std::uint32_t connection_send_window_;
    std::uint32_t connection_receive_window_;
    bool started_{};
    bool sent_goaway_{};
    bool received_goaway_{};

    [[nodiscard]] auto write_frame(frame value) -> runtime::task<void>;
    [[nodiscard]] auto read_frame() -> runtime::task<frame>;
    [[nodiscard]] auto next_for_stream(std::uint32_t stream_id) -> runtime::task<frame>;
    [[nodiscard]] auto await_send_credit(std::uint32_t stream_id) -> runtime::task<void>;
    [[nodiscard]] auto apply_control(frame value) -> runtime::task<void>;
    [[nodiscard]] auto send_settings() -> runtime::task<void>;
    [[nodiscard]] auto send_window_update(std::uint32_t stream_id, std::uint32_t increment)
        -> runtime::task<void>;
};

} // namespace liquid::http2
