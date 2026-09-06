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

class compact_reader {
public:
    explicit compact_reader(std::span<const std::byte> input) : input_(input) {}

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

    std::span<const std::byte> input_;
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

auto decompress_page(std::span<const std::byte> input, std::int64_t codec, std::size_t expected)
    -> result<std::vector<std::byte>> {
    if (codec == 0) {
        if (input.size() != expected) {
            return std::unexpected(failure("uncompressed page size mismatch"));
        }
        return std::vector<std::byte>{input.begin(), input.end()};
    }
    if (codec == 1) {
        return std::unexpected(
            error{errc::unsupported, "Parquet Snappy pages are unsupported", "parquet"});
    }
    auto output = std::vector<std::byte>(expected);
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
    if (codec == 6) {
        const auto produced =
            ZSTD_decompress(output.data(), output.size(), input.data(), input.size());
        if (ZSTD_isError(produced) || produced != expected) {
            return std::unexpected(failure("invalid zstd page"));
        }
        return output;
    }
    return std::unexpected(
        error{errc::unsupported,
              "Parquet compression codec " + std::to_string(codec) + " is unsupported",
              "parquet"});
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
        output |= static_cast<std::uint64_t>(value & 0x7fU) << shift;
        if ((value & 0x80U) == 0) {
            return output;
        }
    }
    return std::unexpected(failure("RLE varint overflows"));
}

auto hybrid(std::span<const std::byte> input, std::uint8_t width, std::size_t wanted)
    -> result<std::vector<std::uint64_t>> {
    auto output = std::vector<std::uint64_t>{};
    auto offset = std::size_t{};
    while (output.size() < wanted && offset < input.size()) {
        auto header = varint(input, offset);
        if (!header || *header == 0) {
            return std::unexpected(failure("invalid RLE run"));
        }
        if ((*header & 1U) == 0) {
            const auto count = *header >> 1U;
            const auto byte_count = (width + 7U) / 8U;
            if (offset + byte_count > input.size()) {
                return std::unexpected(failure("truncated RLE value"));
            }
            auto value = std::uint64_t{};
            for (auto index = std::uint8_t{}; index < byte_count; ++index) {
                value |=
                    static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                    << (index * 8U);
            }
            offset += byte_count;
            for (auto index = std::uint64_t{}; index < count && output.size() < wanted; ++index) {
                output.push_back(value);
            }
            continue;
        }
        const auto count = (*header >> 1U) * 8U;
        const auto bits = count * width;
        const auto bytes_needed = (bits + 7U) / 8U;
        if (offset + bytes_needed > input.size()) {
            return std::unexpected(failure("truncated bit-packed run"));
        }
        auto bit = std::uint64_t{};
        for (auto index = std::uint64_t{}; index < count && output.size() < wanted; ++index) {
            auto value = std::uint64_t{};
            for (auto value_bit = std::uint8_t{}; value_bit < width; ++value_bit) {
                const auto source_bit = bit + value_bit;
                const auto source = std::to_integer<std::uint8_t>(
                    input[offset + static_cast<std::size_t>(source_bit / 8U)]);
                value |= static_cast<std::uint64_t>((source >> (source_bit % 8U)) & 1U)
                         << value_bit;
            }
            output.push_back(value);
            bit += width;
        }
        offset += static_cast<std::size_t>(bytes_needed);
    }
    if (output.size() != wanted) {
        return std::unexpected(failure("RLE stream ended early"));
    }
    return output;
}

auto level_stream(std::span<const std::byte> input,
                  std::uint64_t maximum,
                  std::size_t count,
                  std::size_t& consumed) -> result<std::vector<std::uint64_t>> {
    if (maximum == 0) {
        consumed = 0;
        return std::vector<std::uint64_t>(count, 0);
    }
    auto length = little<std::uint32_t>(input, 0);
    if (!length || *length > input.size() - 4) {
        return std::unexpected(failure("invalid level stream length"));
    }
    auto output = hybrid(input.subspan(4, *length), bit_width(maximum), count);
    if (!output) {
        return std::unexpected(output.error());
    }
    consumed = 4 + *length;
    return output;
}

auto plain_values(std::span<const std::byte> input,
                  std::int64_t type,
                  std::size_t count,
                  std::optional<std::size_t> fixed_size = {}) -> result<std::vector<std::string>> {
    auto output = std::vector<std::string>{};
    auto offset = std::size_t{};
    for (auto index = std::size_t{}; index < count; ++index) {
        if (type == 0) {
            if (index / 8 >= input.size()) {
                return std::unexpected(failure("truncated PLAIN boolean"));
            }
            const auto byte = std::to_integer<std::uint8_t>(input[index / 8]);
            output.push_back((byte & (1U << (index % 8))) != 0 ? "true" : "false");
            continue;
        }
        if (type == 1 || type == 4) {
            auto raw = little<std::uint32_t>(input, offset);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            offset += 4;
            output.push_back(type == 1 ? std::to_string(static_cast<std::int32_t>(*raw))
                                       : std::to_string(std::bit_cast<float>(*raw)));
            continue;
        }
        if (type == 2 || type == 5) {
            auto raw = little<std::uint64_t>(input, offset);
            if (!raw) {
                return std::unexpected(raw.error());
            }
            offset += 8;
            output.push_back(type == 2 ? std::to_string(static_cast<std::int64_t>(*raw))
                                       : std::to_string(std::bit_cast<double>(*raw)));
            continue;
        }
        if (type == 3) {
            if (offset + 12 > input.size()) {
                return std::unexpected(failure("truncated PLAIN INT96"));
            }
            output.push_back(hex(input.subspan(offset, 12)));
            offset += 12;
            continue;
        }
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
        output.emplace_back(reinterpret_cast<const char*>(input.data() + offset), size);
        offset += size;
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

auto schema_leaves(const thrift_value& metadata) -> result<std::map<std::string, schema_leaf>> {
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

auto page_values(std::span<const std::byte> page,
                 const thrift_value& header,
                 const schema_leaf& schema,
                 std::vector<std::string>& dictionary) -> result<std::vector<std::string>> {
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
        auto decoded =
            plain_values(page, schema.type, static_cast<std::size_t>(count), schema.fixed_size);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        dictionary = *decoded;
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
    auto repetitions = level_stream(
        page, schema.maximum_repetition, static_cast<std::size_t>(count), repetition_bytes);
    if (!repetitions) {
        return std::unexpected(repetitions.error());
    }
    offset += repetition_bytes;
    auto definition_bytes = std::size_t{};
    auto definitions = level_stream(page.subspan(offset),
                                    schema.maximum_definition,
                                    static_cast<std::size_t>(count),
                                    definition_bytes);
    if (!definitions) {
        return std::unexpected(definitions.error());
    }
    offset += definition_bytes;
    const auto present =
        static_cast<std::size_t>(std::ranges::count(*definitions, schema.maximum_definition));
    auto decoded = std::vector<std::string>{};
    if (encoding == 0) {
        auto values = plain_values(page.subspan(offset), schema.type, present, schema.fixed_size);
        if (!values) {
            return std::unexpected(values.error());
        }
        decoded = std::move(*values);
    } else if (encoding == 2 || encoding == 8) {
        if (offset >= page.size() || dictionary.empty()) {
            return std::unexpected(failure("dictionary data page is invalid"));
        }
        const auto width = std::to_integer<std::uint8_t>(page[offset++]);
        auto indices = hybrid(page.subspan(offset), width, present);
        if (!indices) {
            return std::unexpected(indices.error());
        }
        for (const auto index : *indices) {
            if (index >= dictionary.size()) {
                return std::unexpected(failure("dictionary index exceeds dictionary"));
            }
            decoded.push_back(dictionary[static_cast<std::size_t>(index)]);
        }
    } else if (encoding == 3) {
        auto values = hybrid(page.subspan(offset), bit_width(1), present);
        if (!values) {
            return std::unexpected(values.error());
        }
        for (const auto value : *values) {
            decoded.push_back(std::to_string(value));
        }
    } else {
        return std::unexpected(
            error{errc::unsupported,
                  "Parquet data page encoding " + std::to_string(encoding) + " is unsupported",
                  schema.path});
    }
    auto output = std::vector<std::string>{};
    auto value_index = std::size_t{};
    for (const auto definition : *definitions) {
        if (definition == schema.maximum_definition) {
            output.push_back(decoded[value_index++]);
        } else {
            output.push_back("NULL");
        }
    }
    return output;
}

auto column_values(std::span<const std::byte> file,
                   const thrift_value& metadata,
                   const schema_leaf& schema,
                   std::uint64_t row_count,
                   std::string& opaque_reason) -> result<std::vector<std::byte>> {
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
    auto values = std::vector<std::string>{};
    auto dictionary = std::vector<std::string>{};
    while (produced < row_count) {
        auto reader = compact_reader{file.subspan(offset)};
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
        auto page = decompress_page(compressed, codec, static_cast<std::size_t>(uncompressed_size));
        if (!page) {
            if (page.error().code == errc::unsupported) {
                opaque_reason = page.error().message;
                return std::vector<std::byte>{};
            }
            return std::unexpected(page.error());
        }
        auto decoded = page_values(*page, *header, schema, dictionary);
        if (!decoded) {
            if (decoded.error().code == errc::unsupported) {
                opaque_reason = decoded.error().message;
                return std::vector<std::byte>{};
            }
            return std::unexpected(decoded.error());
        }
        values.insert(values.end(), decoded->begin(), decoded->end());
        produced += decoded->size();
        offset += reader.offset() + static_cast<std::size_t>(compressed_size);
        if (reader.offset() == 0 && compressed_size == 0) {
            return std::unexpected(failure("empty page made no progress", offset));
        }
    }
    auto output = std::vector<std::byte>{};
    for (const auto& value : values) {
        output.insert(output.end(),
                      reinterpret_cast<const std::byte*>(value.data()),
                      reinterpret_cast<const std::byte*>(value.data()) + value.size());
        output.push_back(std::byte{'\n'});
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
        auto reader = compact_reader{blob.subspan(footer_offset, *footer_size)};
        auto metadata = reader.object_value();
        if (!metadata || reader.offset() != *footer_size) {
            return std::unexpected(failure("invalid Thrift footer", footer_offset));
        }
        auto schemas = schema_leaves(*metadata);
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
        auto remaining_bytes = limits.byte_budget;
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
                if (static_cast<std::uint64_t>(decoded_size) > remaining_bytes) {
                    output.truncated = true;
                    output.limits.push_back("Parquet byte budget reached at " + path);
                    return output;
                }
                auto reason = std::string{};
                auto contents =
                    column_values(blob, *column_metadata, schema->second, row_count, reason);
                if (!contents) {
                    return std::unexpected(contents.error());
                }
                if (contents->size() > remaining_bytes) {
                    output.truncated = true;
                    output.limits.push_back("Parquet byte budget reached at " + path);
                    return output;
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
                remaining_bytes -= contents->size();
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
