#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::protobuf {

enum class wire_type : std::uint8_t {
    varint = 0,
    fixed64 = 1,
    length_delimited = 2,
    start_group = 3,
    end_group = 4,
    fixed32 = 5,
};

struct field {
    std::uint32_t number{};
    wire_type type{};
    std::uint64_t integer{};
    std::span<const std::byte> payload;
    std::span<const std::byte> raw;
};

class reader {
public:
    explicit reader(std::span<const std::byte> input) noexcept;

    [[nodiscard]] auto next() -> result<std::optional<field>>;
    [[nodiscard]] auto finished() const noexcept -> bool;

private:
    [[nodiscard]] auto read_varint() -> result<std::uint64_t>;
    [[nodiscard]] auto read_key() -> result<std::pair<std::uint32_t, wire_type>>;
    [[nodiscard]] auto skip_value(wire_type type, std::uint32_t number) -> result<void>;
    [[nodiscard]] auto skip_group(std::uint32_t number) -> result<void>;

    std::span<const std::byte> input_;
    std::size_t position_{};
};

class writer {
public:
    void varint(std::uint32_t number, std::uint64_t value);
    void int32(std::uint32_t number, std::int32_t value);
    void int64(std::uint32_t number, std::int64_t value);
    void sint32(std::uint32_t number, std::int32_t value);
    void sint64(std::uint32_t number, std::int64_t value);
    void boolean(std::uint32_t number, bool value);
    void fixed32(std::uint32_t number, std::uint32_t value);
    void fixed64(std::uint32_t number, std::uint64_t value);
    void bytes(std::uint32_t number, std::span<const std::byte> value);
    void string(std::uint32_t number, std::string_view value);
    void message(std::uint32_t number, std::span<const std::byte> value);
    void packed_varints(std::uint32_t number, std::span<const std::uint64_t> values);
    void unknown(std::span<const std::byte> value);

    [[nodiscard]] auto take() && -> std::vector<std::byte>;
    [[nodiscard]] auto view() const noexcept -> std::span<const std::byte>;

private:
    void key(std::uint32_t number, wire_type type);
    void raw_varint(std::uint64_t value);

    std::vector<std::byte> output_;
};

[[nodiscard]] constexpr auto zigzag_encode(std::int32_t value) noexcept -> std::uint32_t {
    return (static_cast<std::uint32_t>(value) << 1U) ^ static_cast<std::uint32_t>(-(value < 0));
}

[[nodiscard]] constexpr auto zigzag_encode(std::int64_t value) noexcept -> std::uint64_t {
    return (static_cast<std::uint64_t>(value) << 1U) ^ static_cast<std::uint64_t>(-(value < 0));
}

[[nodiscard]] constexpr auto zigzag_decode32(std::uint32_t value) noexcept -> std::int32_t {
    return static_cast<std::int32_t>((value >> 1U) ^ (0U - (value & 1U)));
}

[[nodiscard]] constexpr auto zigzag_decode64(std::uint64_t value) noexcept -> std::int64_t {
    return static_cast<std::int64_t>((value >> 1U) ^ (0ULL - (value & 1ULL)));
}

[[nodiscard]] auto require_varint(const field& input, std::string_view context)
    -> result<std::uint64_t>;
[[nodiscard]] auto require_fixed32(const field& input, std::string_view context)
    -> result<std::uint32_t>;
[[nodiscard]] auto require_fixed64(const field& input, std::string_view context)
    -> result<std::uint64_t>;
[[nodiscard]] auto require_bytes(const field& input, std::string_view context)
    -> result<std::span<const std::byte>>;
[[nodiscard]] auto require_string(const field& input, std::string_view context)
    -> result<std::string>;
[[nodiscard]] auto unpack_varints(const field& input, std::string_view context)
    -> result<std::vector<std::uint64_t>>;
void preserve_unknown(std::vector<std::byte>& output, const field& input);

} // namespace liquid::protobuf
