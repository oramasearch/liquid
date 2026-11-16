// SPDX-License-Identifier: MIT

#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <optional>
#include <string>
#include <utility>
#include <variant>

#include <zlib.h>
#include <zstd.h>

namespace liquid::formats::detail {
namespace {

constexpr auto parquet_magic = std::string_view{"PAR1"};

auto failure(std::string message, std::size_t offset = 0) -> error {
    return {errc::corrupt,
            std::move(message),
            offset == 0 ? "parquet" : "parquet byte " + std::to_string(offset)};
}

// The bytes one unpack may still allocate. Every buffer, collection and decoded value whose size
// comes from the file is charged here before it is allocated, so no declared size or count can
// allocate past `format_limits::byte_budget`. Column bytes the tree keeps stay charged. Other
// memory is charged through a `held_charges` and returned when that holder ends.
class allocation_budget {
public:
    explicit allocation_budget(std::uint64_t bytes) : remaining_(bytes) {}

    // Charges `count` items of `size` bytes, or fails without charging when they do not fit.
    auto charge(std::uint64_t count, std::uint64_t size = 1) -> result<void> {
        if (size != 0 && count > remaining_ / size) {
            exhausted_ = true;
            return std::unexpected(
                error{errc::unsupported, "Parquet byte budget reached", "parquet"});
        }
        remaining_ -= count * size;
        return {};
    }

    auto refund(std::uint64_t bytes) -> void {
        remaining_ += bytes;
    }

    [[nodiscard]] auto remaining() const -> std::uint64_t {
        return remaining_;
    }

    // True once a charge has failed; the decoders' error is then the budget, not the file.
    [[nodiscard]] auto exhausted() const -> bool {
        return exhausted_;
    }

private:
    std::uint64_t remaining_;
    bool exhausted_{};
};

// Charges held for as long as this holder lives; the destructor returns them to the budget. One
// page's levels and values are held per page, since each value they produce is charged again as
// output. The footer, decompressed pages and dictionaries are held for the whole unpack: many
// column chunks can name the same page, and none of them may decode it on a fresh allowance.
class held_charges {
public:
    explicit held_charges(allocation_budget& budget) : budget_(budget) {}
    held_charges(const held_charges&) = delete;
    auto operator=(const held_charges&) -> held_charges& = delete;

    ~held_charges() {
        budget_.refund(held_);
    }

    auto charge(std::uint64_t count, std::uint64_t size = 1) -> result<void> {
        auto charged = budget_.charge(count, size);
        if (charged) {
            held_ += count * size;
        }
        return charged;
    }

private:
    allocation_budget& budget_;
    std::uint64_t held_{};
};

template <class Integer>
auto little(std::span<const std::byte> input, std::size_t offset) -> result<Integer> {
    if (offset > input.size() || input.size() - offset < sizeof(Integer)) {
        return std::unexpected(failure("truncated integer", offset));
    }
    auto output = Integer{};
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        output |= static_cast<Integer>(std::to_integer<std::uint8_t>(input[offset + index]))
                  << (index * 8U);
    }
    return output;
}

struct thrift_value {
    using list = std::vector<thrift_value>;
    using object = std::map<std::int16_t, thrift_value>;
    using storage = std::
        variant<std::monostate, bool, std::int64_t, double, std::vector<std::byte>, list, object>;
    storage data;
};

// Reads the Thrift compact protocol. Every collection and binary it builds is charged to
// `charges` before it is allocated, and a declared element count must fit in the bytes left.
class compact_reader {
public:
    compact_reader(std::span<const std::byte> input, held_charges& charges)
        : input_(input), charges_(charges) {}

    auto object_value(std::size_t depth = 0) -> result<thrift_value> {
        if (depth > 64) {
            return std::unexpected(failure("Thrift nesting limit exceeded", offset_));
        }
        auto fields = thrift_value::object{};
        auto previous = std::int16_t{};
        while (true) {
            auto header = byte();
            if (!header) {
                return std::unexpected(header.error());
            }
            if (*header == 0) {
                break;
            }
            const auto type = static_cast<std::uint8_t>(*header & 0x0fU);
            const auto delta = static_cast<std::uint8_t>(*header >> 4U);
            auto id = std::int16_t{};
            if (delta == 0) {
                auto decoded = integer();
                if (!decoded || *decoded < std::numeric_limits<std::int16_t>::min() ||
                    *decoded > std::numeric_limits<std::int16_t>::max()) {
                    return std::unexpected(failure("invalid Thrift field id", offset_));
                }
                id = static_cast<std::int16_t>(*decoded);
            } else {
                id = static_cast<std::int16_t>(previous + delta);
            }
            previous = id;
            auto value = type == 1 || type == 2 ? result<thrift_value>{thrift_value{type == 1}}
                                                : read(type, depth + 1);
            if (!value) {
                return std::unexpected(value.error());
            }
            auto charged = charges_.charge(1, field_node_bytes);
            if (!charged) {
                return std::unexpected(charged.error());
            }
            fields.emplace(id, std::move(*value));
        }
        return thrift_value{std::move(fields)};
    }

    [[nodiscard]] auto offset() const -> std::size_t {
        return offset_;
    }

private:
    auto byte() -> result<std::uint8_t> {
        if (offset_ >= input_.size()) {
            return std::unexpected(failure("truncated Thrift value", offset_));
        }
        return std::to_integer<std::uint8_t>(input_[offset_++]);
    }

    auto variable() -> result<std::uint64_t> {
        auto output = std::uint64_t{};
        for (auto shift = 0U; shift < 64U; shift += 7U) {
            auto value = byte();
            if (!value) {
                return std::unexpected(value.error());
            }
            output |= static_cast<std::uint64_t>(*value & 0x7fU) << shift;
            if ((*value & 0x80U) == 0) {
                return output;
            }
        }
        return std::unexpected(failure("Thrift varint overflows", offset_));
    }

    auto integer() -> result<std::int64_t> {
        auto value = variable();
        if (!value) {
            return std::unexpected(value.error());
        }
        return static_cast<std::int64_t>(
            (*value >> 1U) ^ static_cast<std::uint64_t>(-static_cast<std::int64_t>(*value & 1U)));
    }

    auto binary() -> result<thrift_value> {
        auto length = variable();
        if (!length || *length > input_.size() - std::min(input_.size(), offset_)) {
            return std::unexpected(failure("invalid Thrift binary length", offset_));
        }
        auto charged = charges_.charge(*length);
        if (!charged) {
            return std::unexpected(charged.error());
        }
        auto output =
            std::vector<std::byte>{input_.begin() + offset_, input_.begin() + offset_ + *length};
        offset_ += static_cast<std::size_t>(*length);
        return thrift_value{std::move(output)};
    }

    auto list_value(std::size_t depth) -> result<thrift_value> {
        auto header = byte();
        if (!header) {
            return std::unexpected(header.error());
        }
        const auto type = static_cast<std::uint8_t>(*header & 0x0fU);
        auto size = static_cast<std::uint64_t>(*header >> 4U);
        if (size == 15) {
            auto decoded = variable();
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            size = *decoded;
        }
        if (size > 10'000'000) {
            return std::unexpected(failure("Thrift list limit exceeded", offset_));
        }
        // Every element takes at least one byte, so a count beyond the input is a lie.
        if (size > input_.size() - offset_) {
            return std::unexpected(failure("Thrift list exceeds its input", offset_));
        }
        auto charged = charges_.charge(size, sizeof(thrift_value));
        if (!charged) {
            return std::unexpected(charged.error());
        }
        auto output = thrift_value::list{};
        output.reserve(static_cast<std::size_t>(size));
        for (auto index = std::uint64_t{}; index < size; ++index) {
            auto value = read(type, depth + 1);
            if (!value) {
                return std::unexpected(value.error());
            }
            output.push_back(std::move(*value));
        }
        return thrift_value{std::move(output)};
    }

    auto map_value(std::size_t depth) -> result<thrift_value> {
        auto size = variable();
        if (!size) {
            return std::unexpected(size.error());
        }
        if (*size == 0) {
            return thrift_value{thrift_value::list{}};
        }
        auto types = byte();
        if (!types) {
            return std::unexpected(types.error());
        }
        // Every key and every value takes at least one byte.
        if (*size > (input_.size() - offset_) / 2) {
            return std::unexpected(failure("Thrift map exceeds its input", offset_));
        }
        auto charged = charges_.charge(*size * 2, sizeof(thrift_value));
        if (!charged) {
            return std::unexpected(charged.error());
        }
        auto output = thrift_value::list{};
        output.reserve(static_cast<std::size_t>(*size) * 2);
        for (auto index = std::uint64_t{}; index < *size; ++index) {
            auto key = read(*types >> 4U, depth + 1);
            auto value = read(*types & 0x0fU, depth + 1);
            if (!key || !value) {
                return std::unexpected(failure("invalid Thrift map", offset_));
            }
            output.push_back(std::move(*key));
            output.push_back(std::move(*value));
        }
        return thrift_value{std::move(output)};
    }

    auto read(std::uint8_t type, std::size_t depth) -> result<thrift_value> {
        switch (type) {
        case 1:
        case 2: {
            auto value = byte();
            if (!value) {
                return std::unexpected(value.error());
            }
            return thrift_value{*value == 1};
        }
        case 3: {
            auto value = byte();
            if (!value) {
                return std::unexpected(value.error());
            }
            return thrift_value{static_cast<std::int64_t>(static_cast<std::int8_t>(*value))};
        }
        case 4:
        case 5:
        case 6: {
            auto value = integer();
            if (!value) {
                return std::unexpected(value.error());
            }
            return thrift_value{*value};
        }
        case 7: {
            auto value = little<std::uint64_t>(input_, offset_);
            if (!value) {
                return std::unexpected(value.error());
            }
            offset_ += 8;
            return thrift_value{std::bit_cast<double>(*value)};
        }
        case 8:
            return binary();
        case 9:
        case 10:
            return list_value(depth);
        case 11:
            return map_value(depth);
        case 12:
            return object_value(depth);
        default:
            return std::unexpected(failure("unknown Thrift compact type", offset_));
        }
    }

    // One field of a Thrift struct: the map node holding it, with its tree links.
    static constexpr auto field_node_bytes =
        sizeof(thrift_value::object::value_type) + 4 * sizeof(void*);

    std::span<const std::byte> input_;
    held_charges& charges_;
    std::size_t offset_{};
};

auto object(const thrift_value& value) -> const thrift_value::object* {
    return std::get_if<thrift_value::object>(&value.data);
}

auto list(const thrift_value& value) -> const thrift_value::list* {
    return std::get_if<thrift_value::list>(&value.data);
}

auto child(const thrift_value& value, std::int16_t id) -> const thrift_value* {
    const auto* fields = object(value);
    if (!fields) {
        return nullptr;
    }
    const auto found = fields->find(id);
    return found == fields->end() ? nullptr : &found->second;
}

auto integer(const thrift_value& value, std::int16_t id) -> std::optional<std::int64_t> {
    const auto* field = child(value, id);
    const auto* number = field ? std::get_if<std::int64_t>(&field->data) : nullptr;
    return number ? std::optional<std::int64_t>{*number} : std::nullopt;
}

auto binary(const thrift_value& value, std::int16_t id) -> const std::vector<std::byte>* {
    const auto* field = child(value, id);
    return field ? std::get_if<std::vector<std::byte>>(&field->data) : nullptr;
}

auto text(const thrift_value& value, std::int16_t id) -> std::optional<std::string> {
    const auto* value_bytes = binary(value, id);
    if (!value_bytes) {
        return std::nullopt;
    }
    return std::string{reinterpret_cast<const char*>(value_bytes->data()), value_bytes->size()};
}

auto integer_list(const thrift_value& value, std::int16_t id) -> std::vector<std::int64_t> {
    auto output = std::vector<std::int64_t>{};
    const auto* field = child(value, id);
    const auto* values = field ? list(*field) : nullptr;
    if (!values) {
        return output;
    }
    for (const auto& item : *values) {
        if (const auto* number = std::get_if<std::int64_t>(&item.data)) {
            output.push_back(*number);
        }
    }
    return output;
}

auto text_list(const thrift_value& value, std::int16_t id) -> std::vector<std::string> {
    auto output = std::vector<std::string>{};
    const auto* field = child(value, id);
    const auto* values = field ? list(*field) : nullptr;
    if (!values) {
        return output;
    }
    for (const auto& item : *values) {
        if (const auto* raw = std::get_if<std::vector<std::byte>>(&item.data)) {
            output.emplace_back(reinterpret_cast<const char*>(raw->data()), raw->size());
        }
    }
    return output;
}

auto join_path(std::span<const std::string> path) -> std::string {
    auto output = std::string{};
    for (const auto& part : path) {
        if (!output.empty()) {
            output.push_back('/');
        }
        output += part;
    }
    return output;
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    for (const auto value : input) {
        const auto byte = std::to_integer<std::uint8_t>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

// Decodes one page into a buffer of the header's declared size, charged to `charges` before it
// is allocated.
auto decompress_page(std::span<const std::byte> input,
                     std::int64_t codec,
                     std::uint64_t expected,
                     held_charges& charges) -> result<std::vector<std::byte>> {
    if (codec == 1) {
        return std::unexpected(
            error{errc::unsupported, "Parquet Snappy pages are unsupported", "parquet"});
    }
    if (codec != 0 && codec != 2 && codec != 6) {
        return std::unexpected(
            error{errc::unsupported,
                  "Parquet compression codec " + std::to_string(codec) + " is unsupported",
                  "parquet"});
    }
    if (codec == 0 && input.size() != expected) {
        return std::unexpected(failure("uncompressed page size mismatch"));
    }
    auto charged = charges.charge(expected);
    if (!charged) {
        return std::unexpected(charged.error());
    }
    if (codec == 0) {
        return std::vector<std::byte>{input.begin(), input.end()};
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(expected));
    if (codec == 2) {
        auto stream = z_stream{};
        stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
        stream.avail_in = static_cast<uInt>(input.size());
        stream.next_out = reinterpret_cast<Bytef*>(output.data());
        stream.avail_out = static_cast<uInt>(output.size());
        if (inflateInit2(&stream, 16 + MAX_WBITS) != Z_OK) {
            return std::unexpected(
                error{errc::internal, "cannot initialize gzip page decoder", "parquet"});
        }
        const auto status = inflate(&stream, Z_FINISH);
        inflateEnd(&stream);
        if (status != Z_STREAM_END || stream.total_out != expected) {
            return std::unexpected(failure("invalid gzip page"));
        }
        return output;
    }
    const auto produced = ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(produced) || produced != expected) {
        return std::unexpected(failure("invalid zstd page"));
    }
    return output;
}

auto bit_width(std::uint64_t maximum) -> std::uint8_t {
    auto output = std::uint8_t{};
    while (maximum != 0) {
        ++output;
        maximum >>= 1U;
    }
    return output;
}

auto varint(std::span<const std::byte> input, std::size_t& offset) -> result<std::uint64_t> {
    auto output = std::uint64_t{};
    for (auto shift = 0U; shift < 64U; shift += 7U) {
        if (offset >= input.size()) {
            return std::unexpected(failure("truncated RLE varint"));
        }
        const auto value = std::to_integer<std::uint8_t>(input[offset++]);
        // The tenth byte carries only bit 63; anything above it would be silently dropped.
        if (shift == 63U && (value & 0x7eU) != 0) {
            return std::unexpected(failure("RLE varint overflows"));
        }
        output |= static_cast<std::uint64_t>(value & 0x7fU) << shift;
        if ((value & 0x80U) == 0) {
            return output;
        }
    }
    return std::unexpected(failure("RLE varint overflows"));
}

// Decodes `wanted` values from a Parquet RLE/bit-packed hybrid stream. Every run is checked
// against the bytes that remain before it is read, and each read goes through that run's span.
// The output is charged to `charges` before it is allocated: one short RLE run can expand to
// every value the page header declares.
auto hybrid(std::span<const std::byte> input,
            std::uint8_t width,
            std::size_t wanted,
            held_charges& charges) -> result<std::vector<std::uint64_t>> {
    if (width > 64U) {
        return std::unexpected(failure("invalid RLE bit width"));
    }
    auto charged = charges.charge(wanted, sizeof(std::uint64_t));
    if (!charged) {
        return std::unexpected(charged.error());
    }
    auto output = std::vector<std::uint64_t>{};
    output.reserve(wanted);
    auto offset = std::size_t{};
    while (output.size() < wanted && offset < input.size()) {
        auto header = varint(input, offset);
        if (!header || *header == 0) {
            return std::unexpected(failure("invalid RLE run"));
        }
        if ((*header & 1U) == 0) {
            const auto count = *header >> 1U;
            const auto byte_count = (width + 7U) / 8U;
            if (byte_count > input.size() - offset) {
                return std::unexpected(failure("truncated RLE value"));
            }
            const auto run = input.subspan(offset, byte_count);
            auto value = std::uint64_t{};
            for (auto index = std::size_t{}; index < run.size(); ++index) {
                value |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(run[index]))
                         << (index * 8U);
            }
            offset += run.size();
            for (auto index = std::uint64_t{}; index < count && output.size() < wanted; ++index) {
                output.push_back(value);
            }
            continue;
        }
        // Each group of eight values fills exactly `width` bytes, so the run length is bounded
        // by division before it is multiplied.
        const auto groups = *header >> 1U;
        if (width != 0 && groups > (input.size() - offset) / width) {
            return std::unexpected(failure("truncated bit-packed run"));
        }
        const auto run = input.subspan(offset, static_cast<std::size_t>(groups * width));
        auto bit = std::size_t{};
        for (auto index = std::uint64_t{}; index / 8U < groups && output.size() < wanted; ++index) {
            auto value = std::uint64_t{};
            for (auto value_bit = std::uint8_t{}; value_bit < width; ++value_bit) {
                const auto source_bit = bit + value_bit;
                const auto source = std::to_integer<std::uint8_t>(run[source_bit / 8U]);
                value |= static_cast<std::uint64_t>((source >> (source_bit % 8U)) & 1U)
                         << value_bit;
            }
            output.push_back(value);
            bit += width;
        }
        offset += run.size();
    }
    if (output.size() != wanted) {
        return std::unexpected(failure("RLE stream ended early"));
    }
    return output;
}

auto level_stream(std::span<const std::byte> input,
                  std::uint64_t maximum,
                  std::size_t count,
                  std::size_t& consumed,
                  held_charges& charges) -> result<std::vector<std::uint64_t>> {
    if (maximum == 0) {
        auto charged = charges.charge(count, sizeof(std::uint64_t));
        if (!charged) {
            return std::unexpected(charged.error());
        }
        consumed = 0;
        return std::vector<std::uint64_t>(count, 0);
    }
    auto length = little<std::uint32_t>(input, 0);
    if (!length || *length > input.size() - 4) {
        return std::unexpected(failure("invalid level stream length"));
    }
    auto output = hybrid(input.subspan(4, *length), bit_width(maximum), count, charges);
    if (!output) {
        return std::unexpected(output.error());
    }
    consumed = 4 + *length;
    return output;
}

// Decodes `count` PLAIN values, charging each to `charges`: a zero-width FIXED_LEN_BYTE_ARRAY
// reads no input at all, so the count alone cannot be trusted.
auto plain_values(std::span<const std::byte> input,
                  std::int64_t type,
                  std::size_t count,
                  std::optional<std::size_t> fixed_size,
                  held_charges& charges) -> result<std::vector<std::string>> {
    auto charged = charges.charge(count, sizeof(std::string));
    if (!charged) {
        return std::unexpected(charged.error());
    }
    auto output = std::vector<std::string>{};
    output.reserve(count);
    auto offset = std::size_t{};
    for (auto index = std::size_t{}; index < count; ++index) {
        auto value = std::string{};
        if (type == 0) {
            if (index / 8 >= input.size()) {
                return std::unexpected(failure("truncated PLAIN boolean"));
            }
            const auto byte = std::to_integer<std::uint8_t>(input[index / 8]);
            value = (byte & (1U << (index % 8))) != 0 ? "true" : "false";
        } else if (type == 1 || type == 4) {
            auto raw = little<std::uint32_t>(input, offset);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            offset += 4;
            value = type == 1 ? std::to_string(static_cast<std::int32_t>(*raw))
                              : std::to_string(std::bit_cast<float>(*raw));
        } else if (type == 2 || type == 5) {
            auto raw = little<std::uint64_t>(input, offset);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            offset += 8;
            value = type == 2 ? std::to_string(static_cast<std::int64_t>(*raw))
                              : std::to_string(std::bit_cast<double>(*raw));
        } else if (type == 3) {
            if (offset + 12 > input.size()) {
                return std::unexpected(failure("truncated PLAIN INT96"));
            }
            value = hex(input.subspan(offset, 12));
            offset += 12;
        } else {
            auto size = std::size_t{};
            if (type == 6) {
                auto length = little<std::uint32_t>(input, offset);
                if (!length) {
                    return std::unexpected(length.error());
                }
                offset += 4;
                size = *length;
            } else if (type == 7 && fixed_size) {
                size = *fixed_size;
            } else {
                return std::unexpected(error{errc::unsupported,
                                             "Parquet PLAIN physical type is unsupported",
                                             std::to_string(type)});
            }
            if (size > input.size() - std::min(input.size(), offset)) {
                return std::unexpected(failure("truncated PLAIN byte array"));
            }
            charged = charges.charge(size);
            if (!charged) {
                return std::unexpected(charged.error());
            }
            output.emplace_back(reinterpret_cast<const char*>(input.data() + offset), size);
            offset += size;
            continue;
        }
        // Text decoded from a fixed-width value is a few hundred bytes at most, so it is charged
        // once it exists.
        charged = charges.charge(value.size());
        if (!charged) {
            return std::unexpected(charged.error());
        }
        output.push_back(std::move(value));
    }
    return output;
}

struct schema_leaf {
    std::string path;
    std::int64_t type{};
    std::size_t fixed_size{};
    std::uint64_t maximum_definition{};
    std::uint64_t maximum_repetition{};
};

// Walks the flattened schema into its leaves. Nesting is limited like the Thrift reader's, and
// every leaf's path is charged to `charges`: a long group name is copied into each leaf below it.
auto schema_leaves(const thrift_value& metadata, held_charges& charges)
    -> result<std::map<std::string, schema_leaf>> {
    const auto* schema_value = child(metadata, 2);
    const auto* elements = schema_value ? list(*schema_value) : nullptr;
    if (!elements || elements->empty()) {
        return std::unexpected(failure("Parquet schema is absent"));
    }
    auto output = std::map<std::string, schema_leaf>{};
    auto index = std::size_t{1};
    const auto walk = [&](auto&& self,
                          std::int64_t children,
                          std::vector<std::string> path,
                          std::uint64_t definition,
                          std::uint64_t repetition) -> result<void> {
        for (auto child_index = std::int64_t{}; child_index < children; ++child_index) {
            if (index >= elements->size()) {
                return std::unexpected(failure("truncated Parquet schema"));
            }
            if (path.size() >= 64) {
                return std::unexpected(failure("Parquet schema nesting limit exceeded"));
            }
            const auto& element = elements->at(index++);
            auto name = text(element, 4);
            if (!name) {
                return std::unexpected(failure("Parquet schema name is absent"));
            }
            auto next_path = path;
            next_path.push_back(*name);
            const auto repetition_type = integer(element, 3).value_or(0);
            const auto next_definition =
                definition + (repetition_type == 1 || repetition_type == 2 ? 1 : 0);
            const auto next_repetition = repetition + (repetition_type == 2 ? 1 : 0);
            const auto nested = integer(element, 5).value_or(0);
            if (nested > 0) {
                auto walked =
                    self(self, nested, std::move(next_path), next_definition, next_repetition);
                if (!walked) {
                    return walked;
                }
                continue;
            }
            auto type = integer(element, 1);
            if (!type) {
                return std::unexpected(failure("Parquet leaf type is absent"));
            }
            // The map key and the leaf each keep the whole path.
            auto path_size = next_path.size() - 1;
            for (const auto& part : next_path) {
                path_size += part.size();
            }
            auto charged = charges.charge(2, path_size + sizeof(schema_leaf));
            if (!charged) {
                return std::unexpected(charged.error());
            }
            const auto path_text = join_path(next_path);
            output[path_text] = {.path = path_text,
                                 .type = *type,
                                 .fixed_size =
                                     static_cast<std::size_t>(integer(element, 2).value_or(0)),
                                 .maximum_definition = next_definition,
                                 .maximum_repetition = next_repetition};
        }
        return {};
    };
    const auto root_children = integer(elements->front(), 5).value_or(0);
    auto walked = walk(walk, root_children, {}, 0, 0);
    if (!walked) {
        return std::unexpected(walked.error());
    }
    return output;
}

// Decodes one page. Its levels and values are charged to `page_charges`; a dictionary page's
// values are charged to `unpack_charges`.
auto page_values(std::span<const std::byte> page,
                 const thrift_value& header,
                 const schema_leaf& schema,
                 std::vector<std::string>& dictionary,
                 held_charges& page_charges,
                 held_charges& unpack_charges) -> result<std::vector<std::string>> {
    const auto page_type = integer(header, 1).value_or(-1);
    if (page_type == 2) {
        const auto* dictionary_header = child(header, 7);
        if (!dictionary_header) {
            return std::unexpected(failure("dictionary page header is absent"));
        }
        const auto count = integer(*dictionary_header, 1).value_or(-1);
        const auto encoding = integer(*dictionary_header, 2).value_or(-1);
        if (count < 0 || encoding != 0) {
            return std::unexpected(error{
                errc::unsupported, "Parquet dictionary page encoding is unsupported", "parquet"});
        }
        auto decoded = plain_values(
            page, schema.type, static_cast<std::size_t>(count), schema.fixed_size, unpack_charges);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        dictionary = std::move(*decoded);
        return std::vector<std::string>{};
    }
    if (page_type != 0) {
        return std::unexpected(error{
            errc::unsupported, "Parquet page type is unsupported", std::to_string(page_type)});
    }
    const auto* data_header = child(header, 5);
    if (!data_header) {
        return std::unexpected(failure("data page header is absent"));
    }
    const auto count = integer(*data_header, 1).value_or(-1);
    const auto encoding = integer(*data_header, 2).value_or(-1);
    if (count < 0) {
        return std::unexpected(failure("invalid data page count"));
    }
    auto offset = std::size_t{};
    auto repetition_bytes = std::size_t{};
    auto repetitions = level_stream(page,
                                    schema.maximum_repetition,
                                    static_cast<std::size_t>(count),
                                    repetition_bytes,
                                    page_charges);
    if (!repetitions) {
        return std::unexpected(repetitions.error());
    }
    offset += repetition_bytes;
    auto definition_bytes = std::size_t{};
    auto definitions = level_stream(page.subspan(offset),
                                    schema.maximum_definition,
                                    static_cast<std::size_t>(count),
                                    definition_bytes,
                                    page_charges);
    if (!definitions) {
        return std::unexpected(definitions.error());
    }
    offset += definition_bytes;
    const auto present =
        static_cast<std::size_t>(std::ranges::count(*definitions, schema.maximum_definition));
    auto decoded = std::vector<std::string>{};
    if (encoding == 0) {
        auto values = plain_values(
            page.subspan(offset), schema.type, present, schema.fixed_size, page_charges);
        if (!values) {
            return std::unexpected(values.error());
        }
        decoded = std::move(*values);
    } else if (encoding == 2 || encoding == 8) {
        if (offset >= page.size() || dictionary.empty()) {
            return std::unexpected(failure("dictionary data page is invalid"));
        }
        const auto width = std::to_integer<std::uint8_t>(page[offset++]);
        auto indices = hybrid(page.subspan(offset), width, present, page_charges);
        if (!indices) {
            return std::unexpected(indices.error());
        }
        auto charged = page_charges.charge(indices->size(), sizeof(std::string));
        if (!charged) {
            return std::unexpected(charged.error());
        }
        decoded.reserve(indices->size());
        for (const auto index : *indices) {
            if (index >= dictionary.size()) {
                return std::unexpected(failure("dictionary index exceeds dictionary"));
            }
            // Every index copies its dictionary value, so one long value repeated is charged
            // each time it is copied.
            const auto& value = dictionary[static_cast<std::size_t>(index)];
            charged = page_charges.charge(value.size());
            if (!charged) {
                return std::unexpected(charged.error());
            }
            decoded.push_back(value);
        }
    } else if (encoding == 3) {
        auto values = hybrid(page.subspan(offset), bit_width(1), present, page_charges);
        if (!values) {
            return std::unexpected(values.error());
        }
        auto charged = page_charges.charge(values->size(), sizeof(std::string));
        if (!charged) {
            return std::unexpected(charged.error());
        }
        decoded.reserve(values->size());
        for (const auto value : *values) {
            decoded.push_back(std::to_string(value));
        }
    } else {
        return std::unexpected(
            error{errc::unsupported,
                  "Parquet data page encoding " + std::to_string(encoding) + " is unsupported",
                  schema.path});
    }
    auto charged = page_charges.charge(definitions->size(), sizeof(std::string));
    if (!charged) {
        return std::unexpected(charged.error());
    }
    auto output = std::vector<std::string>{};
    output.reserve(definitions->size());
    auto value_index = std::size_t{};
    for (const auto definition : *definitions) {
        if (definition == schema.maximum_definition) {
            output.push_back(std::move(decoded[value_index++]));
        } else {
            output.push_back("NULL");
        }
    }
    return output;
}

// Decodes one column chunk into newline-separated values. The bytes it returns are charged to
// `budget` as they are written. Decompressed pages and the dictionary are charged to
// `unpack_charges`; each page's levels and values are returned once the page is done.
auto column_values(std::span<const std::byte> file,
                   const thrift_value& metadata,
                   const schema_leaf& schema,
                   std::uint64_t row_count,
                   std::string& opaque_reason,
                   allocation_budget& budget,
                   held_charges& unpack_charges) -> result<std::vector<std::byte>> {
    const auto codec = integer(metadata, 4).value_or(-1);
    if (codec == 1) {
        opaque_reason = "Snappy compression is unsupported";
        return std::vector<std::byte>{};
    }
    const auto data_offset = integer(metadata, 9).value_or(-1);
    const auto dictionary_offset = integer(metadata, 11);
    const auto start = dictionary_offset && *dictionary_offset >= 0
                           ? std::min(*dictionary_offset, data_offset)
                           : data_offset;
    if (start < 0 || static_cast<std::uint64_t>(start) >= file.size()) {
        return std::unexpected(failure("invalid column page offset"));
    }
    auto offset = static_cast<std::size_t>(start);
    auto produced = std::uint64_t{};
    auto output = std::vector<std::byte>{};
    // An unsupported page makes the column opaque and returns what it had decoded to the budget;
    // a spent budget is reported by the caller.
    const auto opaque = [&](const error& failed) -> result<std::vector<std::byte>> {
        if (failed.code == errc::unsupported && !budget.exhausted()) {
            opaque_reason = failed.message;
            budget.refund(output.size());
            return std::vector<std::byte>{};
        }
        return std::unexpected(failed);
    };
    auto dictionary = std::vector<std::string>{};
    while (produced < row_count) {
        auto page_charges = held_charges{budget};
        auto reader = compact_reader{file.subspan(offset), page_charges};
        auto header = reader.object_value();
        if (!header) {
            return std::unexpected(header.error());
        }
        const auto compressed_size = integer(*header, 3).value_or(-1);
        const auto uncompressed_size = integer(*header, 2).value_or(-1);
        if (compressed_size < 0 || uncompressed_size < 0 ||
            reader.offset() + compressed_size > file.size() - offset) {
            return std::unexpected(failure("invalid page size", offset));
        }
        const auto compressed =
            file.subspan(offset + reader.offset(), static_cast<std::size_t>(compressed_size));
        auto page = decompress_page(
            compressed, codec, static_cast<std::uint64_t>(uncompressed_size), unpack_charges);
        if (!page) {
            return opaque(page.error());
        }
        auto decoded =
            page_values(*page, *header, schema, dictionary, page_charges, unpack_charges);
        if (!decoded) {
            return opaque(decoded.error());
        }
        for (const auto& value : *decoded) {
            auto charged = budget.charge(value.size() + 1);
            if (!charged) {
                return std::unexpected(charged.error());
            }
            output.insert(output.end(),
                          reinterpret_cast<const std::byte*>(value.data()),
                          reinterpret_cast<const std::byte*>(value.data()) + value.size());
            output.push_back(std::byte{'\n'});
        }
        produced += decoded->size();
        offset += reader.offset() + static_cast<std::size_t>(compressed_size);
        if (reader.offset() == 0 && compressed_size == 0) {
            return std::unexpected(failure("empty page made no progress", offset));
        }
    }
    return output;
}

auto statistics_text(const thrift_value& metadata) -> std::string {
    const auto* statistics = child(metadata, 12);
    if (!statistics) {
        return "";
    }
    const auto* minimum = binary(*statistics, 6);
    const auto* maximum = binary(*statistics, 5);
    const auto nulls = integer(*statistics, 3).value_or(0);
    return "min=" + (minimum ? hex(*minimum) : "") + ";max=" + (maximum ? hex(*maximum) : "") +
           ";nulls=" + std::to_string(nulls);
}

class parquet_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        if (blob.size() < 4) {
            return std::optional<format_recognition>{};
        }
        if (std::string_view{reinterpret_cast<const char*>(blob.data()), 4} != parquet_magic) {
            return std::optional<format_recognition>{};
        }
        if (blob.size() < 12) {
            return std::unexpected(failure("truncated footer"));
        }
        if (std::string_view{reinterpret_cast<const char*>(blob.data() + blob.size() - 4), 4} !=
            parquet_magic) {
            return std::unexpected(failure("footer magic is absent", blob.size() - 4));
        }
        return std::optional<format_recognition>{format_recognition{
            format_tier::semantic_verbatim, "Parquet writer layout is not canonical"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        if (blob.size() < 12 ||
            std::string_view{reinterpret_cast<const char*>(blob.data()), 4} != parquet_magic ||
            std::string_view{reinterpret_cast<const char*>(blob.data() + blob.size() - 4), 4} !=
                parquet_magic) {
            return std::unexpected(failure("invalid Parquet framing"));
        }
        auto footer_size = little<std::uint32_t>(blob, blob.size() - 8);
        if (!footer_size || *footer_size > blob.size() - 12) {
            return std::unexpected(failure("invalid footer size", blob.size() - 8));
        }
        const auto footer_offset = blob.size() - 8 - *footer_size;
        // One budget covers the footer, every page and every column this unpack decodes.
        auto budget = allocation_budget{limits.byte_budget};
        auto unpack_charges = held_charges{budget};
        auto reader = compact_reader{blob.subspan(footer_offset, *footer_size), unpack_charges};
        auto metadata = reader.object_value();
        if (!metadata && budget.exhausted()) {
            return std::unexpected(metadata.error());
        }
        if (!metadata || reader.offset() != *footer_size) {
            return std::unexpected(failure("invalid Thrift footer", footer_offset));
        }
        auto schemas = schema_leaves(*metadata, unpack_charges);
        if (!schemas) {
            return std::unexpected(schemas.error());
        }
        const auto* row_groups_value = child(*metadata, 4);
        const auto* row_groups = row_groups_value ? list(*row_groups_value) : nullptr;
        if (!row_groups) {
            return std::unexpected(failure("row groups are absent"));
        }
        auto output = format_tree{.codec = "parquet",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {blob.begin(), blob.end()}};
        for (auto group_index = std::size_t{}; group_index < row_groups->size(); ++group_index) {
            const auto& group = row_groups->at(group_index);
            const auto row_count = integer(group, 3).value_or(-1);
            const auto* columns_value = child(group, 1);
            const auto* columns = columns_value ? list(*columns_value) : nullptr;
            if (row_count < 0 || !columns) {
                return std::unexpected(failure("invalid row group"));
            }
            for (const auto& column : *columns) {
                const auto* column_metadata = child(column, 3);
                if (!column_metadata) {
                    return std::unexpected(failure("column metadata is absent"));
                }
                const auto path = join_path(text_list(*column_metadata, 3));
                const auto schema = schemas->find(path);
                if (schema == schemas->end()) {
                    return std::unexpected(failure("column schema path is absent"));
                }
                const auto decoded_size = integer(*column_metadata, 6).value_or(-1);
                if (decoded_size < 0) {
                    return std::unexpected(failure("column decoded size is absent"));
                }
                if (static_cast<std::uint64_t>(decoded_size) > budget.remaining()) {
                    output.truncated = true;
                    output.limits.push_back("Parquet byte budget reached at " + path);
                    return output;
                }
                auto reason = std::string{};
                auto contents = column_values(blob,
                                              *column_metadata,
                                              schema->second,
                                              row_count,
                                              reason,
                                              budget,
                                              unpack_charges);
                if (!contents) {
                    if (budget.exhausted()) {
                        output.truncated = true;
                        output.limits.push_back("Parquet byte budget reached at " + path);
                        return output;
                    }
                    return std::unexpected(contents.error());
                }
                auto metadata_bytes = attributes(
                    {{"codec", std::to_string(integer(*column_metadata, 4).value_or(-1))},
                     {"encodings",
                      [&] {
                          auto encoded = std::string{};
                          for (const auto value : integer_list(*column_metadata, 2)) {
                              if (!encoded.empty()) {
                                  encoded.push_back(',');
                              }
                              encoded += std::to_string(value);
                          }
                          return encoded;
                      }()},
                     {"rows", std::to_string(row_count)},
                     {"statistics", statistics_text(*column_metadata)}});
                if (!metadata_bytes) {
                    return std::unexpected(metadata_bytes.error());
                }
                auto entry = make_entry("row-group=" + std::to_string(group_index) + "/" + path,
                                        format_entry_kind::dataset,
                                        std::move(*contents),
                                        std::move(*metadata_bytes));
                if (!entry) {
                    return std::unexpected(entry.error());
                }
                entry->opaque_reason = std::move(reason);
                output.entries.push_back(std::move(*entry));
                if (output.entries.size() > limits.maximum_entries) {
                    output.truncated = true;
                    output.limits.push_back("Parquet entry limit reached");
                    return output;
                }
            }
        }
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return volatile_fields_;
    }

private:
    static constexpr auto volatile_fields_ = std::array<std::string_view, 1>{"created_by"};
};

constexpr auto extensions = std::array<const char*, 1>{".parquet"};
constexpr auto magic_bytes = std::array<std::uint8_t, 4>{'P', 'A', 'R', '1'};
constexpr auto magic = std::array<liquid_format_magic_v1, 1>{
    liquid_format_magic_v1{magic_bytes.data(), magic_bytes.size()}};
constexpr auto volatile_names = std::array<const char*, 1>{"created_by"};

} // namespace

auto parquet_module() -> const liquid_format_module_v1* {
    static auto backend = parquet_backend{};
    static const auto module =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = "parquet",
                                               .tier = 2,
                                               .extensions = extensions.data(),
                                               .extension_count = extensions.size(),
                                               .magic = magic.data(),
                                               .magic_count = magic.size(),
                                               .volatile_fields = volatile_names.data(),
                                               .volatile_field_count = volatile_names.size(),
                                               .limits = {.maximum_depth = 8,
                                                          .byte_budget = 256ULL * 1024ULL * 1024ULL,
                                                          .maximum_entries = 100'000}},
                                .module_context = &backend,
                                .recognise = recognise_operation,
                                .unpack = unpack_operation,
                                .canonicalise = canonicalise_operation,
                                .diff = diff_operation,
                                .merge = merge_operation,
                                .repack = nullptr,
                                .destroy = nullptr};
    return &module;
}

} // namespace liquid::formats::detail
