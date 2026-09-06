#include "formats/builtin.hpp"

#include <algorithm>
#include <array>
#include <bit>
#include <cmath>
#include <limits>
#include <map>
#include <set>
#include <string>
#include <utility>

#include <zlib.h>

#include "encoding/cbor.hpp"

namespace liquid::formats::detail {
namespace {

namespace cbor = encoding::cbor;

constexpr auto file_signature = std::array<std::byte, 8>{std::byte{0x89},
                                                         std::byte{'H'},
                                                         std::byte{'D'},
                                                         std::byte{'F'},
                                                         std::byte{'\r'},
                                                         std::byte{'\n'},
                                                         std::byte{0x1a},
                                                         std::byte{'\n'}};
constexpr auto undefined_address = std::numeric_limits<std::uint64_t>::max();

auto failure(std::string message, std::uint64_t offset = 0) -> error {
    return {errc::corrupt,
            std::move(message),
            offset == 0 ? "hdf5" : "hdf5 byte " + std::to_string(offset)};
}

auto attribute_text(const format_entry& entry, std::string_view name) -> std::string {
    auto decoded = cbor::decode(entry.attributes);
    if (!decoded) {
        return {};
    }
    const auto* fields = std::get_if<cbor::value::map>(&decoded->data);
    if (!fields) {
        return {};
    }
    for (const auto& [key, value] : *fields) {
        const auto* key_text = std::get_if<std::string>(&key.data);
        const auto* value_text = std::get_if<std::string>(&value.data);
        if (key_text && value_text && *key_text == name) {
            return *value_text;
        }
    }
    return {};
}

template <class Integer> auto little_value(std::span<const std::byte> input) -> Integer {
    auto output = Integer{};
    for (auto index = std::size_t{}; index < sizeof(Integer); ++index) {
        output |= static_cast<Integer>(std::to_integer<std::uint8_t>(input[index])) << (index * 8U);
    }
    return output;
}

auto close_number(double left, double right, format_diff_options options) -> bool {
    if (std::isnan(left) || std::isnan(right)) {
        return std::isnan(left) && std::isnan(right);
    }
    if (left == right) {
        return true;
    }
    const auto tolerance = options.absolute_tolerance +
                           options.relative_tolerance * std::max(std::abs(left), std::abs(right));
    return std::isfinite(left) && std::isfinite(right) && std::abs(left - right) <= tolerance;
}

auto float_values_equal(const format_entry& left,
                        const format_entry& right,
                        format_diff_options options) -> bool {
    if (left.attributes != right.attributes || left.bytes.size() != right.bytes.size()) {
        return false;
    }
    const auto dtype = attribute_text(left, "dtype");
    const auto width = dtype == "float:4"   ? std::size_t{4}
                       : dtype == "float:8" ? std::size_t{8}
                                            : std::size_t{};
    if (width == 0 || left.bytes.size() % width != 0) {
        return false;
    }
    for (auto offset = std::size_t{}; offset < left.bytes.size(); offset += width) {
        auto old_value = double{};
        auto new_value = double{};
        if (width == 4) {
            old_value = std::bit_cast<float>(little_value<std::uint32_t>(
                std::span<const std::byte>{left.bytes}.subspan(offset, width)));
            new_value = std::bit_cast<float>(little_value<std::uint32_t>(
                std::span<const std::byte>{right.bytes}.subspan(offset, width)));
        } else {
            old_value = std::bit_cast<double>(little_value<std::uint64_t>(
                std::span<const std::byte>{left.bytes}.subspan(offset, width)));
            new_value = std::bit_cast<double>(little_value<std::uint64_t>(
                std::span<const std::byte>{right.bytes}.subspan(offset, width)));
        }
        if (!close_number(old_value, new_value, options)) {
            return false;
        }
    }
    return true;
}

auto little(std::span<const std::byte> input, std::size_t offset, std::size_t size)
    -> result<std::uint64_t> {
    if (size == 0 || size > 8 || offset > input.size() || size > input.size() - offset) {
        return std::unexpected(failure("truncated integer", offset));
    }
    auto output = std::uint64_t{};
    for (auto index = std::size_t{}; index < size; ++index) {
        output |= static_cast<std::uint64_t>(std::to_integer<std::uint8_t>(input[offset + index]))
                  << (index * 8U);
    }
    return output;
}

auto text_at(std::span<const std::byte> input, std::size_t offset) -> result<std::string> {
    if (offset >= input.size()) {
        return std::unexpected(failure("string offset exceeds file", offset));
    }
    auto end = offset;
    while (end < input.size() && input[end] != std::byte{}) {
        ++end;
    }
    if (end == input.size()) {
        return std::unexpected(failure("unterminated string", offset));
    }
    return std::string{reinterpret_cast<const char*>(input.data() + offset), end - offset};
}

auto aligned(std::size_t value, std::size_t alignment) -> std::size_t {
    return (value + alignment - 1) / alignment * alignment;
}

struct message {
    std::uint16_t type{};
    std::vector<std::byte> data;
};

struct object_header {
    std::vector<message> messages;
};

struct filter {
    std::uint16_t id{};
    std::vector<std::uint32_t> values;
};

struct layout {
    std::uint8_t kind{};
    std::uint8_t dimensions{};
    std::uint64_t address{undefined_address};
    std::uint64_t size{};
    std::vector<std::uint32_t> chunks;
    std::vector<std::byte> compact;
    std::string unsupported;
};

struct dataset {
    std::vector<std::uint64_t> dimensions;
    std::string dtype;
    std::uint64_t element_size{};
    layout storage;
    std::vector<filter> filters;
};

class hdf5_file {
public:
    hdf5_file(std::span<const std::byte> bytes, format_limits limits)
        : bytes_(bytes), limits_(limits) {}

    auto open() -> result<void> {
        if (bytes_.size() < 9 || !std::ranges::equal(file_signature, bytes_.first(8))) {
            return std::unexpected(failure("invalid file signature"));
        }
        version_ = std::to_integer<std::uint8_t>(bytes_[8]);
        if (version_ > 3) {
            return std::unexpected(error{errc::unsupported,
                                         "HDF5 superblock version is unsupported",
                                         std::to_string(version_)});
        }
        if (version_ <= 1) {
            if (bytes_.size() < 24) {
                return std::unexpected(failure("truncated superblock"));
            }
            offset_size_ = std::to_integer<std::uint8_t>(bytes_[13]);
            length_size_ = std::to_integer<std::uint8_t>(bytes_[14]);
            auto base = little(bytes_, 24, offset_size_);
            auto end = little(bytes_, 24 + offset_size_ * 2, offset_size_);
            auto root = little(bytes_, 24 + offset_size_ * 4 + offset_size_, offset_size_);
            if (!base || !end || !root) {
                return std::unexpected(failure("truncated version 0 superblock"));
            }
            base_ = *base;
            end_ = address(*end);
            root_ = address(*root);
        } else {
            if (bytes_.size() < 12) {
                return std::unexpected(failure("truncated superblock"));
            }
            offset_size_ = std::to_integer<std::uint8_t>(bytes_[9]);
            length_size_ = std::to_integer<std::uint8_t>(bytes_[10]);
            auto base = little(bytes_, 12, offset_size_);
            auto end = little(bytes_, 12 + offset_size_ * 2, offset_size_);
            auto root = little(bytes_, 12 + offset_size_ * 3, offset_size_);
            if (!base || !end || !root) {
                return std::unexpected(failure("truncated version 2 superblock"));
            }
            base_ = *base;
            end_ = address(*end);
            root_ = address(*root);
        }
        if ((offset_size_ != 2 && offset_size_ != 4 && offset_size_ != 8) ||
            (length_size_ != 2 && length_size_ != 4 && length_size_ != 8) ||
            root_ >= bytes_.size() || end_ > bytes_.size()) {
            return std::unexpected(failure("invalid superblock sizes"));
        }
        return {};
    }

    auto tree() -> result<format_tree> {
        auto output = format_tree{.codec = "hdf5",
                                  .tier = format_tier::semantic_verbatim,
                                  .original = {bytes_.begin(), bytes_.end()}};
        auto visited = std::set<std::uint64_t>{};
        auto walked = walk_object(root_, "", output, visited);
        if (!walked) {
            return std::unexpected(walked.error());
        }
        return output;
    }

private:
    auto address(std::uint64_t raw) const -> std::uint64_t {
        if (raw == undefined_address || raw > undefined_address - base_) {
            return undefined_address;
        }
        return base_ + raw;
    }

    auto region(std::uint64_t offset, std::uint64_t size) const
        -> result<std::span<const std::byte>> {
        if (offset > bytes_.size() || size > bytes_.size() - static_cast<std::size_t>(offset)) {
            return std::unexpected(failure("range exceeds file", offset));
        }
        return bytes_.subspan(static_cast<std::size_t>(offset), static_cast<std::size_t>(size));
    }

    auto header_v1(std::uint64_t offset) -> result<object_header> {
        auto prefix = region(offset, 16);
        if (!prefix) {
            return std::unexpected(prefix.error());
        }
        if (std::to_integer<std::uint8_t>((*prefix)[0]) != 1) {
            return std::unexpected(failure("invalid object header version", offset));
        }
        auto count = little(*prefix, 2, 2);
        auto size = little(*prefix, 8, 4);
        if (!count || !size) {
            return std::unexpected(failure("truncated object header", offset));
        }
        auto chunk = region(offset + 16, *size);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        auto output = object_header{};
        auto position = std::size_t{};
        for (auto index = std::uint64_t{}; index < *count; ++index) {
            if (position + 8 > chunk->size()) {
                return std::unexpected(failure("truncated object message", offset + 16 + position));
            }
            auto type = little(*chunk, position, 2);
            auto message_size = little(*chunk, position + 2, 2);
            if (!type || !message_size || position + 8 + *message_size > chunk->size()) {
                return std::unexpected(failure("invalid object message", offset + 16 + position));
            }
            const auto data = chunk->subspan(position + 8, static_cast<std::size_t>(*message_size));
            if (*type != 0) {
                output.messages.push_back(
                    {static_cast<std::uint16_t>(*type), {data.begin(), data.end()}});
            }
            position += 8 + aligned(static_cast<std::size_t>(*message_size), 8);
        }
        return output;
    }

    auto header_v2(std::uint64_t offset) -> result<object_header> {
        auto prefix = region(offset, 7);
        if (!prefix ||
            std::string_view{reinterpret_cast<const char*>(prefix->data()), 4} != "OHDR") {
            return std::unexpected(failure("invalid version 2 object header", offset));
        }
        if (std::to_integer<std::uint8_t>((*prefix)[4]) != 2) {
            return std::unexpected(failure("invalid version 2 object header", offset));
        }
        const auto flags = std::to_integer<std::uint8_t>((*prefix)[5]);
        auto position = std::size_t{6};
        if ((flags & 0x20U) != 0) {
            position += 16;
        }
        if ((flags & 0x10U) != 0) {
            position += 4;
        }
        const auto size_width = std::size_t{1} << (flags & 0x03U);
        auto chunk_size = little(bytes_, offset + position, size_width);
        if (!chunk_size) {
            return std::unexpected(chunk_size.error());
        }
        position += size_width;
        auto chunk = region(offset + position, *chunk_size);
        if (!chunk) {
            return std::unexpected(chunk.error());
        }
        auto output = object_header{};
        auto cursor = std::size_t{};
        while (cursor + 4 <= chunk->size()) {
            const auto type = std::to_integer<std::uint8_t>((*chunk)[cursor]);
            auto message_size = little(*chunk, cursor + 1, 2);
            if (!message_size || cursor + 4 + *message_size > chunk->size()) {
                break;
            }
            auto header_size = std::size_t{4};
            if ((flags & 0x04U) != 0) {
                header_size += 2;
            }
            if (cursor + header_size + *message_size > chunk->size()) {
                return std::unexpected(
                    failure("truncated version 2 message", offset + position + cursor));
            }
            if (type != 0) {
                const auto data = chunk->subspan(cursor + header_size, *message_size);
                output.messages.push_back({type, {data.begin(), data.end()}});
            }
            cursor += header_size + static_cast<std::size_t>(*message_size);
        }
        return output;
    }

    auto object(std::uint64_t offset) -> result<object_header> {
        if (offset >= bytes_.size()) {
            return std::unexpected(failure("object address exceeds file", offset));
        }
        if (bytes_[offset] == std::byte{1}) {
            return header_v1(offset);
        }
        return header_v2(offset);
    }

    auto message_of(const object_header& header, std::uint16_t type) const -> const message* {
        const auto found = std::ranges::find(header.messages, type, &message::type);
        return found == header.messages.end() ? nullptr : &*found;
    }

    auto dataspace_message(const message& source) -> result<std::vector<std::uint64_t>> {
        if (source.data.size() < 4) {
            return std::unexpected(failure("truncated dataspace message"));
        }
        const auto version = std::to_integer<std::uint8_t>(source.data[0]);
        const auto rank = std::to_integer<std::uint8_t>(source.data[1]);
        const auto flags = std::to_integer<std::uint8_t>(source.data[2]);
        const auto start = version == 1 ? std::size_t{8} : std::size_t{4};
        if ((version != 1 && version != 2) || rank > 32 ||
            start + static_cast<std::size_t>(rank) * length_size_ > source.data.size()) {
            return std::unexpected(failure("invalid dataspace message"));
        }
        auto output = std::vector<std::uint64_t>{};
        for (auto index = std::uint8_t{}; index < rank; ++index) {
            auto dimension = little(source.data, start + index * length_size_, length_size_);
            if (!dimension) {
                return std::unexpected(dimension.error());
            }
            output.push_back(*dimension);
        }
        if ((flags & 0x01U) != 0 &&
            start + static_cast<std::size_t>(rank) * length_size_ * 2 > source.data.size()) {
            return std::unexpected(failure("truncated maximum dimensions"));
        }
        return output;
    }

    auto datatype_message(const message& source) -> result<std::pair<std::string, std::uint64_t>> {
        if (source.data.size() < 8) {
            return std::unexpected(failure("truncated datatype message"));
        }
        const auto type_class = std::to_integer<std::uint8_t>(source.data[0]) & 0x0fU;
        auto size = little(source.data, 4, 4);
        if (!size || *size == 0) {
            return std::unexpected(failure("invalid datatype size"));
        }
        auto name = std::string{};
        switch (type_class) {
        case 0:
            name = "fixed-point";
            break;
        case 1:
            name = "float";
            break;
        case 3:
            name = "string";
            break;
        default:
            name = "opaque-class-" + std::to_string(type_class);
            break;
        }
        return std::pair{name + ":" + std::to_string(*size), *size};
    }

    auto layout_message(const message& source) -> result<layout> {
        if (source.data.size() < 2) {
            return std::unexpected(failure("truncated layout message"));
        }
        const auto version = std::to_integer<std::uint8_t>(source.data[0]);
        auto output = layout{.kind = std::to_integer<std::uint8_t>(source.data[1])};
        if (version != 3) {
            output.unsupported = "layout message version " + std::to_string(version);
            return output;
        }
        if (output.kind == 0) {
            auto size = little(source.data, 2, 2);
            if (!size || 4 + *size > source.data.size()) {
                return std::unexpected(failure("invalid compact layout"));
            }
            output.size = *size;
            output.compact.assign(source.data.begin() + 4, source.data.begin() + 4 + *size);
            return output;
        }
        if (output.kind == 1) {
            auto address_value = little(source.data, 2, offset_size_);
            auto size = little(source.data, 2 + offset_size_, length_size_);
            if (!address_value || !size) {
                return std::unexpected(failure("invalid contiguous layout"));
            }
            output.address = address(*address_value);
            output.size = *size;
            return output;
        }
        if (output.kind == 2) {
            if (source.data.size() < 3 + offset_size_) {
                return std::unexpected(failure("truncated chunked layout"));
            }
            output.dimensions = std::to_integer<std::uint8_t>(source.data[2]);
            auto address_value = little(source.data, 3, offset_size_);
            if (!address_value || output.dimensions == 0 ||
                3 + offset_size_ + static_cast<std::size_t>(output.dimensions) * 4 >
                    source.data.size()) {
                return std::unexpected(failure("invalid chunked layout"));
            }
            output.address = address(*address_value);
            for (auto index = std::uint8_t{}; index < output.dimensions; ++index) {
                auto dimension = little(source.data, 3 + offset_size_ + index * 4, 4);
                if (!dimension) {
                    return std::unexpected(dimension.error());
                }
                output.chunks.push_back(static_cast<std::uint32_t>(*dimension));
            }
            return output;
        }
        output.unsupported = "layout class " + std::to_string(output.kind);
        return output;
    }

    auto filter_message(const message& source) -> result<std::vector<filter>> {
        if (source.data.size() < 2) {
            return std::unexpected(failure("truncated filter message"));
        }
        const auto version = std::to_integer<std::uint8_t>(source.data[0]);
        const auto count = std::to_integer<std::uint8_t>(source.data[1]);
        auto position = version == 1 ? std::size_t{8} : std::size_t{2};
        auto output = std::vector<filter>{};
        for (auto index = std::uint8_t{}; index < count; ++index) {
            auto id = little(source.data, position, 2);
            if (!id) {
                return std::unexpected(id.error());
            }
            position += 2;
            auto name_size = std::uint64_t{};
            if (version == 1 || *id >= 256) {
                auto decoded = little(source.data, position, 2);
                if (!decoded) {
                    return std::unexpected(decoded.error());
                }
                name_size = *decoded;
                position += 2;
            }
            auto flags = little(source.data, position, 2);
            auto values = little(source.data, position + 2, 2);
            if (!flags || !values) {
                return std::unexpected(failure("truncated filter descriptor"));
            }
            position += 4;
            if (name_size != 0) {
                const auto padded = aligned(static_cast<std::size_t>(name_size), 8);
                if (position + padded > source.data.size()) {
                    return std::unexpected(failure("truncated filter name"));
                }
                position += padded;
            }
            auto current = filter{static_cast<std::uint16_t>(*id), {}};
            for (auto value_index = std::uint64_t{}; value_index < *values; ++value_index) {
                auto value = little(source.data, position, 4);
                if (!value) {
                    return std::unexpected(value.error());
                }
                current.values.push_back(static_cast<std::uint32_t>(*value));
                position += 4;
            }
            if (version == 1 && (*values % 2) != 0) {
                position += 4;
            }
            if (position > source.data.size()) {
                return std::unexpected(failure("filter descriptor exceeds message"));
            }
            output.push_back(std::move(current));
        }
        return output;
    }

    auto decode_filters(std::vector<std::byte> input,
                        std::span<const filter> filters,
                        std::uint32_t mask,
                        std::uint64_t expected) -> result<std::vector<std::byte>> {
        for (auto index = filters.size(); index != 0; --index) {
            const auto filter_index = index - 1;
            if ((mask & (std::uint32_t{1} << filter_index)) != 0) {
                continue;
            }
            const auto& current = filters[filter_index];
            if (current.id == 1) {
                if (expected > std::numeric_limits<uLongf>::max()) {
                    return std::unexpected(
                        error{errc::unsupported, "HDF5 chunk exceeds zlib limits", "hdf5"});
                }
                auto output = std::vector<std::byte>(static_cast<std::size_t>(expected));
                auto size = static_cast<uLongf>(output.size());
                const auto status = uncompress(reinterpret_cast<Bytef*>(output.data()),
                                               &size,
                                               reinterpret_cast<const Bytef*>(input.data()),
                                               input.size());
                if (status != Z_OK || size != output.size()) {
                    return std::unexpected(failure("invalid deflate chunk"));
                }
                input = std::move(output);
            } else if (current.id == 2) {
                const auto element_size =
                    current.values.empty() ? std::size_t{1} : current.values.front();
                if (element_size == 0 || input.size() % element_size != 0) {
                    return std::unexpected(failure("invalid shuffled chunk"));
                }
                const auto elements = input.size() / element_size;
                auto output = std::vector<std::byte>(input.size());
                for (auto element = std::size_t{}; element < elements; ++element) {
                    for (auto byte = std::size_t{}; byte < element_size; ++byte) {
                        output[element * element_size + byte] = input[byte * elements + element];
                    }
                }
                input = std::move(output);
            } else {
                return std::unexpected(
                    error{errc::unsupported,
                          "HDF5 filter " + std::to_string(current.id) + " is unsupported",
                          "hdf5"});
            }
        }
        return input;
    }

    struct chunk_record {
        std::vector<std::uint64_t> offsets;
        std::uint32_t size{};
        std::uint32_t mask{};
        std::uint64_t address{};
    };

    auto chunks(std::uint64_t node,
                std::uint8_t dimensions,
                std::vector<chunk_record>& output,
                std::set<std::uint64_t>& visited) -> result<void> {
        if (!visited.insert(node).second) {
            return std::unexpected(failure("chunk B-tree cycle", node));
        }
        auto header = region(node, 8 + offset_size_ * 2);
        if (!header ||
            std::string_view{reinterpret_cast<const char*>(header->data()), 4} != "TREE" ||
            std::to_integer<std::uint8_t>((*header)[4]) != 1) {
            return std::unexpected(failure("invalid chunk B-tree node", node));
        }
        const auto level = std::to_integer<std::uint8_t>((*header)[5]);
        auto entries = little(*header, 6, 2);
        if (!entries || *entries > limits_.maximum_entries) {
            return std::unexpected(failure("invalid chunk B-tree entry count", node));
        }
        const auto key_size = std::size_t{8} + static_cast<std::size_t>(dimensions) * 8;
        auto position = std::size_t{8} + offset_size_ * 2;
        for (auto index = std::uint64_t{}; index < *entries; ++index) {
            auto record_bytes = region(node + position, key_size + offset_size_);
            if (!record_bytes) {
                return std::unexpected(record_bytes.error());
            }
            auto size = little(*record_bytes, 0, 4);
            auto mask = little(*record_bytes, 4, 4);
            if (!size || !mask) {
                return std::unexpected(failure("invalid chunk key", node + position));
            }
            auto record = chunk_record{.size = static_cast<std::uint32_t>(*size),
                                       .mask = static_cast<std::uint32_t>(*mask)};
            for (auto dimension = std::uint8_t{}; dimension < dimensions; ++dimension) {
                auto offset = little(*record_bytes, 8 + dimension * 8, 8);
                if (!offset) {
                    return std::unexpected(offset.error());
                }
                record.offsets.push_back(*offset);
            }
            auto child = little(*record_bytes, key_size, offset_size_);
            if (!child) {
                return std::unexpected(child.error());
            }
            record.address = address(*child);
            if (level == 0) {
                output.push_back(std::move(record));
            } else {
                auto nested = chunks(record.address, dimensions, output, visited);
                if (!nested) {
                    return nested;
                }
            }
            position += key_size + offset_size_;
        }
        return {};
    }

    auto dataset_bytes(const dataset& value) -> result<std::vector<std::byte>> {
        if (!value.storage.unsupported.empty()) {
            return std::unexpected(
                error{errc::unsupported, value.storage.unsupported, "hdf5 dataset"});
        }
        if (value.storage.kind == 0) {
            if (value.storage.compact.size() > remaining_bytes_) {
                return std::unexpected(
                    error{errc::unsupported, "HDF5 dataset exceeds byte budget", "hdf5"});
            }
            remaining_bytes_ -= value.storage.compact.size();
            return value.storage.compact;
        }
        if (value.storage.kind == 1) {
            if (value.storage.size > remaining_bytes_) {
                return std::unexpected(
                    error{errc::unsupported, "HDF5 dataset exceeds byte budget", "hdf5"});
            }
            auto contents = region(value.storage.address, value.storage.size);
            if (!contents) {
                return std::unexpected(contents.error());
            }
            remaining_bytes_ -= value.storage.size;
            return std::vector<std::byte>{contents->begin(), contents->end()};
        }
        if (value.storage.kind != 2 || value.storage.chunks.empty()) {
            return std::unexpected(
                error{errc::unsupported, "HDF5 dataset layout is unsupported", "hdf5"});
        }
        auto expected = value.element_size;
        for (auto index = std::size_t{};
             index < value.storage.chunks.size() && index < value.dimensions.size();
             ++index) {
            if (value.storage.chunks[index] != 0 &&
                expected > limits_.byte_budget / value.storage.chunks[index]) {
                return std::unexpected(
                    error{errc::unsupported, "HDF5 chunk exceeds byte budget", "hdf5"});
            }
            expected *= value.storage.chunks[index];
        }
        auto records = std::vector<chunk_record>{};
        auto visited = std::set<std::uint64_t>{};
        auto read = chunks(value.storage.address, value.storage.dimensions, records, visited);
        if (!read) {
            return std::unexpected(read.error());
        }
        auto total_elements = std::uint64_t{1};
        for (const auto dimension : value.dimensions) {
            if (dimension != 0 && total_elements > limits_.byte_budget / dimension) {
                return std::unexpected(
                    error{errc::unsupported, "HDF5 dataset exceeds byte budget", "hdf5"});
            }
            total_elements *= dimension;
        }
        if (value.element_size != 0 && total_elements > limits_.byte_budget / value.element_size) {
            return std::unexpected(
                error{errc::unsupported, "HDF5 dataset exceeds byte budget", "hdf5"});
        }
        const auto output_size = total_elements * value.element_size;
        if (output_size > remaining_bytes_) {
            return std::unexpected(
                error{errc::unsupported, "HDF5 dataset exceeds byte budget", "hdf5"});
        }
        auto output = std::vector<std::byte>(static_cast<std::size_t>(output_size));
        for (const auto& record : records) {
            auto encoded = region(record.address, record.size);
            if (!encoded) {
                return std::unexpected(encoded.error());
            }
            auto decoded = decode_filters(std::vector<std::byte>{encoded->begin(), encoded->end()},
                                          value.filters,
                                          record.mask,
                                          expected);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            const auto rank = value.dimensions.size();
            if (record.offsets.size() < rank || value.storage.chunks.size() < rank ||
                decoded->size() != expected) {
                return std::unexpected(failure("invalid decoded chunk dimensions"));
            }
            auto chunk_elements = std::uint64_t{1};
            for (auto dimension = std::size_t{}; dimension < rank; ++dimension) {
                chunk_elements *= value.storage.chunks[dimension];
            }
            for (auto element = std::uint64_t{}; element < chunk_elements; ++element) {
                auto remaining = element;
                auto destination = std::uint64_t{};
                auto stride = std::uint64_t{1};
                auto inside = true;
                for (auto dimension = rank; dimension != 0; --dimension) {
                    const auto index = dimension - 1;
                    const auto chunk_dimension = value.storage.chunks[index];
                    const auto local = remaining % chunk_dimension;
                    remaining /= chunk_dimension;
                    const auto global = record.offsets[index] + local;
                    if (global >= value.dimensions[index]) {
                        inside = false;
                    }
                    destination += global * stride;
                    stride *= value.dimensions[index];
                }
                if (!inside) {
                    continue;
                }
                const auto source_offset = static_cast<std::size_t>(element * value.element_size);
                const auto destination_offset =
                    static_cast<std::size_t>(destination * value.element_size);
                std::ranges::copy_n(decoded->begin() + source_offset,
                                    static_cast<std::size_t>(value.element_size),
                                    output.begin() + destination_offset);
            }
        }
        remaining_bytes_ -= output_size;
        return output;
    }

    auto decode_dataset(const object_header& header) -> result<dataset> {
        const auto* dataspace = message_of(header, 1);
        const auto* datatype = message_of(header, 3);
        const auto* layout_source = message_of(header, 8);
        if (!dataspace || !datatype || !layout_source) {
            return std::unexpected(failure("dataset messages are incomplete"));
        }
        auto dimensions = dataspace_message(*dataspace);
        auto dtype = datatype_message(*datatype);
        auto storage = layout_message(*layout_source);
        if (!dimensions || !dtype || !storage) {
            return std::unexpected(failure("dataset message is invalid"));
        }
        auto output = dataset{.dimensions = std::move(*dimensions),
                              .dtype = std::move(dtype->first),
                              .element_size = dtype->second,
                              .storage = std::move(*storage)};
        if (const auto* pipeline = message_of(header, 11)) {
            auto decoded = filter_message(*pipeline);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            output.filters = std::move(*decoded);
        }
        return output;
    }

    auto heap(std::uint64_t offset) -> result<std::span<const std::byte>> {
        auto header = region(offset, 8 + length_size_ * 2 + offset_size_);
        if (!header ||
            std::string_view{reinterpret_cast<const char*>(header->data()), 4} != "HEAP") {
            return std::unexpected(failure("invalid local heap", offset));
        }
        auto data_size = little(*header, 8, length_size_);
        auto data_address = little(*header, 8 + length_size_ * 2, offset_size_);
        if (!data_size || !data_address) {
            return std::unexpected(failure("truncated local heap", offset));
        }
        return region(address(*data_address), *data_size);
    }

    auto symbol_node(std::uint64_t offset,
                     std::span<const std::byte> names,
                     std::vector<std::pair<std::string, std::uint64_t>>& output) -> result<void> {
        auto header = region(offset, 8);
        if (!header ||
            std::string_view{reinterpret_cast<const char*>(header->data()), 4} != "SNOD") {
            return std::unexpected(failure("invalid symbol table node", offset));
        }
        auto count = little(*header, 6, 2);
        if (!count || *count > limits_.maximum_entries) {
            return std::unexpected(failure("invalid symbol count", offset));
        }
        const auto entry_size = offset_size_ * 2 + 24;
        for (auto index = std::uint64_t{}; index < *count; ++index) {
            auto entry = region(offset + 8 + index * entry_size, entry_size);
            if (!entry) {
                return std::unexpected(entry.error());
            }
            auto name_offset = little(*entry, 0, offset_size_);
            auto object_address = little(*entry, offset_size_, offset_size_);
            if (!name_offset || !object_address) {
                return std::unexpected(failure("invalid symbol entry", offset));
            }
            auto name = text_at(names, *name_offset);
            if (!name) {
                return std::unexpected(name.error());
            }
            output.emplace_back(std::move(*name), address(*object_address));
        }
        return {};
    }

    auto group_btree(std::uint64_t offset,
                     std::span<const std::byte> names,
                     std::vector<std::pair<std::string, std::uint64_t>>& output,
                     std::set<std::uint64_t>& visited) -> result<void> {
        if (!visited.insert(offset).second) {
            return std::unexpected(failure("group B-tree cycle", offset));
        }
        auto header = region(offset, 8 + offset_size_ * 2);
        if (!header ||
            std::string_view{reinterpret_cast<const char*>(header->data()), 4} != "TREE" ||
            std::to_integer<std::uint8_t>((*header)[4]) != 0) {
            return std::unexpected(failure("invalid group B-tree", offset));
        }
        const auto level = std::to_integer<std::uint8_t>((*header)[5]);
        auto count = little(*header, 6, 2);
        if (!count || *count > limits_.maximum_entries) {
            return std::unexpected(failure("invalid group B-tree count", offset));
        }
        auto position = std::size_t{8} + offset_size_ * 2;
        for (auto index = std::uint64_t{}; index < *count; ++index) {
            auto child = little(bytes_, offset + position + offset_size_, offset_size_);
            if (!child) {
                return std::unexpected(child.error());
            }
            const auto child_address = address(*child);
            auto read = level == 0 ? symbol_node(child_address, names, output)
                                   : group_btree(child_address, names, output, visited);
            if (!read) {
                return read;
            }
            position += offset_size_ * 2;
        }
        return {};
    }

    auto old_group_links(const message& symbol_table)
        -> result<std::vector<std::pair<std::string, std::uint64_t>>> {
        if (symbol_table.data.size() < offset_size_ * 2) {
            return std::unexpected(failure("truncated symbol table message"));
        }
        auto btree_address = little(symbol_table.data, 0, offset_size_);
        auto heap_address = little(symbol_table.data, offset_size_, offset_size_);
        if (!btree_address || !heap_address) {
            return std::unexpected(failure("invalid symbol table message"));
        }
        auto names = heap(address(*heap_address));
        if (!names) {
            return std::unexpected(names.error());
        }
        auto output = std::vector<std::pair<std::string, std::uint64_t>>{};
        auto visited = std::set<std::uint64_t>{};
        auto read = group_btree(address(*btree_address), *names, output, visited);
        if (!read) {
            return std::unexpected(read.error());
        }
        return output;
    }

    auto link_message(const message& source) -> result<std::pair<std::string, std::uint64_t>> {
        if (source.data.size() < 2 || source.data[0] != std::byte{1}) {
            return std::unexpected(failure("invalid link message"));
        }
        const auto flags = std::to_integer<std::uint8_t>(source.data[1]);
        auto position = std::size_t{2};
        auto link_type = std::uint8_t{};
        if ((flags & 0x08U) != 0) {
            if (position >= source.data.size()) {
                return std::unexpected(failure("truncated link type"));
            }
            link_type = std::to_integer<std::uint8_t>(source.data[position++]);
        }
        if ((flags & 0x04U) != 0) {
            position += 8;
        }
        if ((flags & 0x10U) != 0) {
            position += 1;
        }
        const auto length_size = std::size_t{1} << (flags & 0x03U);
        auto name_size = little(source.data, position, length_size);
        if (!name_size) {
            return std::unexpected(name_size.error());
        }
        position += length_size;
        if (position + *name_size > source.data.size()) {
            return std::unexpected(failure("truncated link name"));
        }
        auto name = std::string{reinterpret_cast<const char*>(source.data.data() + position),
                                static_cast<std::size_t>(*name_size)};
        position += static_cast<std::size_t>(*name_size);
        if (link_type != 0) {
            return std::unexpected(
                error{errc::unsupported, "only hard HDF5 links are supported", name});
        }
        auto target = little(source.data, position, offset_size_);
        if (!target) {
            return std::unexpected(target.error());
        }
        return std::pair{std::move(name), address(*target)};
    }

    auto group_links(const object_header& header)
        -> result<std::vector<std::pair<std::string, std::uint64_t>>> {
        if (const auto* symbols = message_of(header, 17)) {
            return old_group_links(*symbols);
        }
        auto output = std::vector<std::pair<std::string, std::uint64_t>>{};
        for (const auto& source : header.messages) {
            if (source.type != 6) {
                continue;
            }
            auto link = link_message(source);
            if (!link) {
                return std::unexpected(link.error());
            }
            output.push_back(std::move(*link));
        }
        return output;
    }

    auto dimensions_text(std::span<const std::uint64_t> dimensions) -> std::string {
        auto output = std::string{};
        for (auto index = std::size_t{}; index < dimensions.size(); ++index) {
            if (index != 0) {
                output.push_back('x');
            }
            output += std::to_string(dimensions[index]);
        }
        return output;
    }

    auto walk_object(std::uint64_t offset,
                     std::string path,
                     format_tree& output,
                     std::set<std::uint64_t>& visited) -> result<void> {
        if (!visited.insert(offset).second) {
            return {};
        }
        if (visited.size() > limits_.maximum_entries) {
            output.truncated = true;
            output.limits.push_back("HDF5 object limit reached at " + path);
            return {};
        }
        auto header = object(offset);
        if (!header) {
            return std::unexpected(header.error());
        }
        if (message_of(*header, 1) && message_of(*header, 3) && message_of(*header, 8)) {
            auto decoded = decode_dataset(*header);
            if (!decoded) {
                return std::unexpected(decoded.error());
            }
            auto contents = dataset_bytes(*decoded);
            auto metadata = attributes({{"dimensions", dimensions_text(decoded->dimensions)},
                                        {"dtype", decoded->dtype},
                                        {"layout", std::to_string(decoded->storage.kind)},
                                        {"rank", std::to_string(decoded->dimensions.size())}});
            if (!metadata) {
                return std::unexpected(metadata.error());
            }
            if (!contents) {
                auto opaque =
                    make_entry(path, format_entry_kind::dataset, {}, std::move(*metadata));
                if (!opaque) {
                    return std::unexpected(opaque.error());
                }
                opaque->opaque_reason = contents.error().message;
                output.entries.push_back(std::move(*opaque));
                return {};
            }
            auto entry = make_entry(
                path, format_entry_kind::dataset, std::move(*contents), std::move(*metadata));
            if (!entry) {
                return std::unexpected(entry.error());
            }
            output.entries.push_back(std::move(*entry));
            return {};
        }
        if (!path.empty()) {
            auto group = make_entry(path, format_entry_kind::directory, {});
            if (!group) {
                return std::unexpected(group.error());
            }
            output.entries.push_back(std::move(*group));
        }
        auto links = group_links(*header);
        if (!links) {
            return std::unexpected(links.error());
        }
        for (auto& [name, child] : *links) {
            const auto child_path = path.empty() ? name : path + "/" + name;
            auto walked = walk_object(child, std::move(child_path), output, visited);
            if (!walked) {
                return walked;
            }
        }
        return {};
    }

    std::span<const std::byte> bytes_;
    format_limits limits_;
    std::uint64_t remaining_bytes_{limits_.byte_budget};
    std::uint8_t version_{};
    std::uint8_t offset_size_{};
    std::uint8_t length_size_{};
    std::uint64_t base_{};
    std::uint64_t end_{};
    std::uint64_t root_{};
};

class hdf5_backend final : public codec_backend {
public:
    auto recognise(std::span<const std::byte> blob, std::string_view)
        -> result<std::optional<format_recognition>> override {
        if (blob.size() < file_signature.size()) {
            return std::optional<format_recognition>{};
        }
        if (!std::ranges::equal(file_signature, blob.first(file_signature.size()))) {
            return std::optional<format_recognition>{};
        }
        if (blob.size() < 9) {
            return std::unexpected(failure("truncated superblock"));
        }
        const auto version = std::to_integer<std::uint8_t>(blob[8]);
        if (version > 3) {
            return std::unexpected(error{errc::unsupported,
                                         "HDF5 superblock version is unsupported",
                                         std::to_string(version)});
        }
        return std::optional<format_recognition>{format_recognition{
            format_tier::semantic_verbatim, "HDF5 allocation history is not canonical"}};
    }

    auto unpack(std::span<const std::byte> blob, format_limits limits)
        -> result<format_tree> override {
        auto file = hdf5_file{blob, limits};
        auto opened = file.open();
        if (!opened) {
            return std::unexpected(opened.error());
        }
        return file.tree();
    }

    auto diff(const format_tree& old_tree, const format_tree& new_tree, format_diff_options options)
        -> result<format_diff> override {
        auto output = codec_backend::diff(old_tree, new_tree, options);
        if (!output || (options.absolute_tolerance == 0 && options.relative_tolerance == 0)) {
            return output;
        }
        std::erase_if(output->entries, [&](const format_diff_entry& difference) {
            if (difference.kind != format_diff_kind::modified) {
                return false;
            }
            const auto old_entry =
                std::ranges::find(old_tree.entries, difference.path, &format_entry::path);
            const auto new_entry =
                std::ranges::find(new_tree.entries, difference.path, &format_entry::path);
            return old_entry != old_tree.entries.end() && new_entry != new_tree.entries.end() &&
                   old_entry->kind == format_entry_kind::dataset &&
                   new_entry->kind == format_entry_kind::dataset &&
                   float_values_equal(*old_entry, *new_entry, options);
        });
        return output;
    }

    auto volatile_fields() const -> std::span<const std::string_view> override {
        return volatile_fields_;
    }

private:
    static constexpr auto volatile_fields_ =
        std::array<std::string_view, 2>{"allocation_address", "creation_order"};
};

constexpr auto extensions = std::array<const char*, 4>{".h5", ".hdf5", ".hdf", ".nc"};
constexpr auto magic_bytes =
    std::array<std::uint8_t, 8>{0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n'};
constexpr auto magic = std::array<liquid_format_magic_v1, 1>{
    liquid_format_magic_v1{magic_bytes.data(), magic_bytes.size()}};
constexpr auto volatile_names = std::array<const char*, 2>{"allocation_address", "creation_order"};

} // namespace

auto hdf5_module() -> const liquid_format_module_v1* {
    static auto backend = hdf5_backend{};
    static const auto module =
        liquid_format_module_v1{.abi_version = LIQUID_FORMAT_ABI_VERSION,
                                .struct_size = sizeof(liquid_format_module_v1),
                                .descriptor = {.name = "hdf5",
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
