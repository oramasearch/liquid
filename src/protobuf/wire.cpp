#include "protobuf/wire.hpp"

#include <bit>
#include <limits>
#include <utility>

namespace liquid::protobuf {
namespace {

auto failure(std::string message, std::string_view context = "protobuf") -> error {
    return {errc::corrupt, std::move(message), std::string{context}};
}

auto valid_number(std::uint32_t number) noexcept -> bool {
    return number != 0 && number <= 0x1fffffffU;
}

} // namespace

reader::reader(std::span<const std::byte> input) noexcept : input_(input) {}

auto reader::read_varint() -> result<std::uint64_t> {
    auto value = std::uint64_t{};
    for (auto index = std::uint32_t{}; index < 10; ++index) {
        if (position_ == input_.size()) {
            return std::unexpected(failure("truncated varint"));
        }
        const auto byte = std::to_integer<std::uint8_t>(input_[position_++]);
        if (index == 9 && byte > 1U) {
            return std::unexpected(failure("varint exceeds 64 bits"));
        }
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << (index * 7U);
        if ((byte & 0x80U) == 0) {
            return value;
        }
    }
    return std::unexpected(failure("varint exceeds 10 bytes"));
}

auto reader::read_key() -> result<std::pair<std::uint32_t, wire_type>> {
    auto encoded = read_varint();
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    constexpr auto maximum_key = (std::uint64_t{0x1fffffff} << 3U) | 7U;
    if (*encoded > maximum_key) {
        return std::unexpected(failure("field key exceeds 32 bits"));
    }
    const auto number = static_cast<std::uint32_t>(*encoded >> 3U);
    const auto raw_type = static_cast<std::uint8_t>(*encoded & 0x07U);
    if (!valid_number(number)) {
        return std::unexpected(failure("invalid field number"));
    }
    if (raw_type > static_cast<std::uint8_t>(wire_type::fixed32)) {
        return std::unexpected(failure("invalid wire type"));
    }
    return std::pair{number, static_cast<wire_type>(raw_type)};
}

auto reader::skip_value(wire_type type, std::uint32_t number) -> result<void> {
    switch (type) {
    case wire_type::varint: {
        auto value = read_varint();
        if (!value) {
            return std::unexpected(value.error());
        }
        return {};
    }
    case wire_type::fixed64:
        if (input_.size() - position_ < 8) {
            return std::unexpected(failure("truncated fixed64 field"));
        }
        position_ += 8;
        return {};
    case wire_type::length_delimited: {
        auto length = read_varint();
        if (!length || *length > input_.size() - position_) {
            return std::unexpected(failure("truncated length-delimited field"));
        }
        position_ += static_cast<std::size_t>(*length);
        return {};
    }
    case wire_type::start_group:
        return skip_group(number);
    case wire_type::end_group:
        return std::unexpected(failure("unexpected end-group field"));
    case wire_type::fixed32:
        if (input_.size() - position_ < 4) {
            return std::unexpected(failure("truncated fixed32 field"));
        }
        position_ += 4;
        return {};
    }
    return std::unexpected(failure("invalid wire type"));
}

auto reader::skip_group(std::uint32_t number) -> result<void> {
    auto groups = std::vector<std::uint32_t>{number};
    while (!groups.empty()) {
        if (position_ == input_.size()) {
            return std::unexpected(failure("unterminated group"));
        }
        auto key = read_key();
        if (!key) {
            return std::unexpected(key.error());
        }
        const auto [child_number, type] = *key;
        if (type == wire_type::start_group) {
            groups.push_back(child_number);
            continue;
        }
        if (type == wire_type::end_group) {
            if (groups.back() != child_number) {
                return std::unexpected(failure("mismatched end-group field"));
            }
            groups.pop_back();
            continue;
        }
        auto skipped = skip_value(type, child_number);
        if (!skipped) {
            return skipped;
        }
    }
    return {};
}

auto reader::next() -> result<std::optional<field>> {
    if (position_ == input_.size()) {
        return std::optional<field>{};
    }
    const auto start = position_;
    auto key = read_key();
    if (!key) {
        return std::unexpected(key.error());
    }
    const auto [number, type] = *key;
    auto output = field{.number = number, .type = type};
    switch (type) {
    case wire_type::varint: {
        auto value = read_varint();
        if (!value) {
            return std::unexpected(value.error());
        }
        output.integer = *value;
        break;
    }
    case wire_type::fixed64:
        if (input_.size() - position_ < 8) {
            return std::unexpected(failure("truncated fixed64 field"));
        }
        output.payload = input_.subspan(position_, 8);
        output.integer = 0;
        for (auto index = std::size_t{}; index < 8; ++index) {
            output.integer |=
                static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input_[position_ + index]))
                << (index * 8U);
        }
        position_ += 8;
        break;
    case wire_type::length_delimited: {
        auto length = read_varint();
        if (!length || *length > input_.size() - position_) {
            return std::unexpected(failure("truncated length-delimited field"));
        }
        output.payload = input_.subspan(position_, static_cast<std::size_t>(*length));
        position_ += static_cast<std::size_t>(*length);
        break;
    }
    case wire_type::start_group: {
        auto skipped = skip_group(number);
        if (!skipped) {
            return std::unexpected(skipped.error());
        }
        break;
    }
    case wire_type::end_group:
        return std::unexpected(failure("unexpected end-group field"));
    case wire_type::fixed32:
        if (input_.size() - position_ < 4) {
            return std::unexpected(failure("truncated fixed32 field"));
        }
        output.payload = input_.subspan(position_, 4);
        output.integer = 0;
        for (auto index = std::size_t{}; index < 4; ++index) {
            output.integer |=
                static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input_[position_ + index]))
                << (index * 8U);
        }
        position_ += 4;
        break;
    }
    output.raw = input_.subspan(start, position_ - start);
    return std::optional{output};
}

auto reader::finished() const noexcept -> bool {
    return position_ == input_.size();
}

void writer::raw_varint(std::uint64_t value) {
    while (value >= 0x80U) {
        output_.push_back(std::byte{static_cast<std::uint8_t>((value & 0x7fU) | 0x80U)});
        value >>= 7U;
    }
    output_.push_back(std::byte{static_cast<std::uint8_t>(value)});
}

void writer::key(std::uint32_t number, wire_type type) {
    raw_varint((static_cast<std::uint64_t>(number) << 3U) | static_cast<std::uint8_t>(type));
}

void writer::varint(std::uint32_t number, std::uint64_t value) {
    key(number, wire_type::varint);
    raw_varint(value);
}

void writer::int32(std::uint32_t number, std::int32_t value) {
    varint(number, static_cast<std::uint64_t>(static_cast<std::int64_t>(value)));
}

void writer::int64(std::uint32_t number, std::int64_t value) {
    varint(number, static_cast<std::uint64_t>(value));
}

void writer::sint32(std::uint32_t number, std::int32_t value) {
    varint(number, zigzag_encode(value));
}

void writer::sint64(std::uint32_t number, std::int64_t value) {
    varint(number, zigzag_encode(value));
}

void writer::boolean(std::uint32_t number, bool value) {
    varint(number, value ? 1U : 0U);
}

void writer::fixed32(std::uint32_t number, std::uint32_t value) {
    key(number, wire_type::fixed32);
    for (auto shift = std::uint32_t{}; shift < 32; shift += 8) {
        output_.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

void writer::fixed64(std::uint32_t number, std::uint64_t value) {
    key(number, wire_type::fixed64);
    for (auto shift = std::uint32_t{}; shift < 64; shift += 8) {
        output_.push_back(std::byte{static_cast<std::uint8_t>(value >> shift)});
    }
}

void writer::bytes(std::uint32_t number, std::span<const std::byte> value) {
    key(number, wire_type::length_delimited);
    raw_varint(value.size());
    output_.insert(output_.end(), value.begin(), value.end());
}

void writer::string(std::uint32_t number, std::string_view value) {
    bytes(number, std::span{reinterpret_cast<const std::byte*>(value.data()), value.size()});
}

void writer::message(std::uint32_t number, std::span<const std::byte> value) {
    bytes(number, value);
}

void writer::packed_varints(std::uint32_t number, std::span<const std::uint64_t> values) {
    auto packed = writer{};
    for (const auto value : values) {
        packed.raw_varint(value);
    }
    bytes(number, packed.view());
}

void writer::unknown(std::span<const std::byte> value) {
    output_.insert(output_.end(), value.begin(), value.end());
}

auto writer::take() && -> std::vector<std::byte> {
    return std::move(output_);
}

auto writer::view() const noexcept -> std::span<const std::byte> {
    return output_;
}

auto require_varint(const field& input, std::string_view context) -> result<std::uint64_t> {
    if (input.type != wire_type::varint) {
        return std::unexpected(failure("field is not a varint", context));
    }
    return input.integer;
}

auto require_fixed32(const field& input, std::string_view context) -> result<std::uint32_t> {
    if (input.type != wire_type::fixed32) {
        return std::unexpected(failure("field is not fixed32", context));
    }
    return static_cast<std::uint32_t>(input.integer);
}

auto require_fixed64(const field& input, std::string_view context) -> result<std::uint64_t> {
    if (input.type != wire_type::fixed64) {
        return std::unexpected(failure("field is not fixed64", context));
    }
    return input.integer;
}

auto require_bytes(const field& input, std::string_view context)
    -> result<std::span<const std::byte>> {
    if (input.type != wire_type::length_delimited) {
        return std::unexpected(failure("field is not length-delimited", context));
    }
    return input.payload;
}

auto require_string(const field& input, std::string_view context) -> result<std::string> {
    auto bytes = require_bytes(input, context);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    return std::string{reinterpret_cast<const char*>(bytes->data()), bytes->size()};
}

auto unpack_varints(const field& input, std::string_view context)
    -> result<std::vector<std::uint64_t>> {
    if (input.type == wire_type::varint) {
        return std::vector<std::uint64_t>{input.integer};
    }
    auto bytes = require_bytes(input, context);
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto output = std::vector<std::uint64_t>{};
    auto position = std::size_t{};
    while (position < bytes->size()) {
        auto value = std::uint64_t{};
        auto complete = false;
        for (auto index = std::uint32_t{}; index < 10; ++index) {
            if (position == bytes->size()) {
                return std::unexpected(failure("truncated packed varint", context));
            }
            const auto byte = std::to_integer<std::uint8_t>((*bytes)[position++]);
            if (index == 9 && byte > 1U) {
                return std::unexpected(failure("packed varint exceeds 64 bits", context));
            }
            value |= static_cast<std::uint64_t>(byte & 0x7fU) << (index * 7U);
            if ((byte & 0x80U) == 0) {
                complete = true;
                break;
            }
        }
        if (!complete) {
            return std::unexpected(failure("packed varint exceeds 10 bytes", context));
        }
        output.push_back(value);
    }
    return output;
}

void preserve_unknown(std::vector<std::byte>& output, const field& input) {
    output.insert(output.end(), input.raw.begin(), input.raw.end());
}

} // namespace liquid::protobuf
