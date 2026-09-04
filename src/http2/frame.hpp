#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::http2 {

enum class frame_type : std::uint8_t {
    data = 0,
    headers = 1,
    priority = 2,
    rst_stream = 3,
    settings = 4,
    push_promise = 5,
    ping = 6,
    goaway = 7,
    window_update = 8,
    continuation = 9,
};

struct frame {
    frame_type type{};
    std::uint8_t flags{};
    std::uint32_t stream_id{};
    std::vector<std::byte> payload;
};

inline constexpr std::uint8_t flag_end_stream = 0x1;
inline constexpr std::uint8_t flag_ack = 0x1;
inline constexpr std::uint8_t flag_end_headers = 0x4;
inline constexpr std::uint32_t default_window_size = 65'535;
inline constexpr std::uint32_t default_max_frame_size = 16'384;

[[nodiscard]] auto encode_frame(const frame& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode_frame(std::span<const std::byte> input) -> result<frame>;
[[nodiscard]] auto setting_payload(std::uint16_t id, std::uint32_t value)
    -> std::array<std::byte, 6>;

} // namespace liquid::http2
