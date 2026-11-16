// SPDX-License-Identifier: MIT

#include "git/object_store.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>
#include <set>

#include <zlib.h>

#include "objects/entry_name.hpp"

namespace liquid::git_detail {
namespace {

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

struct decode_budget {
    object_read_limits limits;
    std::uint64_t used{};
    std::optional<std::uint64_t> target;

    auto consume(std::uint64_t size) -> result<void> {
        const auto limit = 2 * target.value_or(limits.object_bytes) + limits.read_slack_bytes;
        if (used > limit || size > limit - used)
            return std::unexpected(failure(errc::corrupt, "Git object read budget exceeded"));
        used += size;
        return {};
    }

    auto set_target(std::uint64_t size) -> result<void> {
        if (!target)
            target = size;
        return consume(0);
    }
};

auto read_file(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    if (!input)
        return std::unexpected(
            failure(errc::not_found, "Git object is not present", path.string()));
    const auto end = input.tellg();
    if (end < 0)
        return std::unexpected(failure(errc::io, "cannot determine file size", path.string()));
    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(bytes.data()), static_cast<std::streamsize>(bytes.size()));
    if (!input && !bytes.empty())
        return std::unexpected(failure(errc::io, "cannot read Git file", path.string()));
    return bytes;
}

auto deflate(std::span<const std::byte> input, std::string_view context)
    -> result<std::vector<std::byte>> {
    uLongf size = compressBound(static_cast<uLong>(input.size()));
    auto output = std::vector<std::byte>(size);
    const auto status = compress2(reinterpret_cast<Bytef*>(output.data()),
                                  &size,
                                  reinterpret_cast<const Bytef*>(input.data()),
                                  static_cast<uLong>(input.size()),
                                  Z_DEFAULT_COMPRESSION);
    if (status != Z_OK)
        return std::unexpected(
            failure(errc::internal, "cannot compress Git object", std::string{context}));
    output.resize(size);
    return output;
}

auto parse_kind(std::string_view value) -> std::optional<object_kind> {
    if (value == "blob")
        return object_kind::blob;
    if (value == "tree")
        return object_kind::tree;
    if (value == "commit")
        return object_kind::commit;
    if (value == "tag")
        return object_kind::tag;
    return std::nullopt;
}

auto object_bytes(object_kind kind, std::span<const std::byte> payload) -> std::vector<std::byte> {
    const auto header = std::string{kind_name(kind)} + " " + std::to_string(payload.size()) + '\0';
    auto output =
        std::vector<std::byte>{reinterpret_cast<const std::byte*>(header.data()),
                               reinterpret_cast<const std::byte*>(header.data() + header.size())};
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

auto trim(std::string value) -> std::string {
    while (!value.empty() && (value.back() == '\n' || value.back() == '\r'))
        value.pop_back();
    return value;
}

auto big_endian32(std::span<const std::byte> bytes) -> std::uint32_t {
    return (static_cast<std::uint32_t>(bytes[0]) << 24U) |
           (static_cast<std::uint32_t>(bytes[1]) << 16U) |
           (static_cast<std::uint32_t>(bytes[2]) << 8U) | static_cast<std::uint32_t>(bytes[3]);
}

auto read_at(std::istream& input,
             const std::filesystem::path& path,
             std::uint64_t offset,
             std::size_t size) -> result<std::vector<std::byte>> {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    auto output = std::vector<std::byte>(size);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input)
        return std::unexpected(failure(errc::corrupt, "truncated Git pack", path.string()));
    return output;
}

auto read_at(const std::filesystem::path& path, std::uint64_t offset, std::size_t size)
    -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        return std::unexpected(failure(errc::not_found, "Git pack is not present", path.string()));
    return read_at(input, path, offset, size);
}

auto pack_offset(const std::filesystem::path& index, const sha1& id)
    -> result<std::optional<std::uint64_t>> {
    const auto header = read_at(index, 0, 8 + 256 * 4);
    if (!header)
        return std::unexpected(header.error());
    if (big_endian32(std::span{*header}.subspan(0, 4)) != 0xff744f63U ||
        big_endian32(std::span{*header}.subspan(4, 4)) != 2U)
        return std::unexpected(
            failure(errc::corrupt, "unsupported Git pack index", index.string()));
    const auto count = big_endian32(std::span{*header}.subspan(8 + 255 * 4, 4));
    const auto first = std::to_integer<unsigned char>(id.front());
    const auto low =
        first == 0 ? 0U : big_endian32(std::span{*header}.subspan(8 + (first - 1) * 4, 4));
    const auto high = big_endian32(std::span{*header}.subspan(8 + first * 4, 4));
    std::uint32_t begin = low;
    std::uint32_t end = high;
    constexpr std::uint64_t names = 8 + 256 * 4;
    while (begin < end) {
        const auto middle = begin + (end - begin) / 2;
        const auto value =
            read_at(index, names + static_cast<std::uint64_t>(middle) * id.size(), id.size());
        if (!value)
            return std::unexpected(value.error());
        if (std::ranges::lexicographical_compare(*value, id)) {
            begin = middle + 1;
        } else if (std::ranges::lexicographical_compare(id, *value)) {
            end = middle;
        } else {
            const auto offset_table = names + static_cast<std::uint64_t>(count) * id.size() +
                                      static_cast<std::uint64_t>(count) * 4;
            const auto raw =
                read_at(index, offset_table + static_cast<std::uint64_t>(middle) * 4, 4);
            if (!raw)
                return std::unexpected(raw.error());
            const auto value_offset = big_endian32(*raw);
            if ((value_offset & 0x80000000U) == 0)
                return std::uint64_t{value_offset};
            const auto large =
                read_at(index,
                        offset_table + static_cast<std::uint64_t>(count) * 4 +
                            static_cast<std::uint64_t>(value_offset & 0x7fffffffU) * 8,
                        8);
            if (!large)
                return std::unexpected(large.error());
            return (static_cast<std::uint64_t>(big_endian32(std::span{*large}.subspan(0, 4)))
                    << 32U) |
                   big_endian32(std::span{*large}.subspan(4, 4));
        }
    }
    return std::optional<std::uint64_t>{};
}

auto scan_pack_id_at_offset(const std::filesystem::path& index, std::uint64_t wanted)
    -> result<std::optional<sha1>> {
    const auto header = read_at(index, 0, 8 + 256 * 4);
    if (!header)
        return std::unexpected(header.error());
    if (big_endian32(std::span{*header}.subspan(0, 4)) != 0xff744f63U ||
        big_endian32(std::span{*header}.subspan(4, 4)) != 2U)
        return std::unexpected(
            failure(errc::corrupt, "unsupported Git pack index", index.string()));
    const auto count = big_endian32(std::span{*header}.subspan(8 + 255 * 4, 4));
    constexpr std::uint64_t names = 8 + 256 * 4;
    const auto offset_table = names + static_cast<std::uint64_t>(count) * sha1{}.size() +
                              static_cast<std::uint64_t>(count) * 4;
    for (std::uint32_t number{}; number < count; ++number) {
        const auto raw = read_at(index, offset_table + static_cast<std::uint64_t>(number) * 4, 4);
        if (!raw)
            return std::unexpected(raw.error());
        const auto short_offset = big_endian32(*raw);
        std::uint64_t offset = short_offset;
        if ((short_offset & 0x80000000U) != 0) {
            const auto large =
                read_at(index,
                        offset_table + static_cast<std::uint64_t>(count) * 4 +
                            static_cast<std::uint64_t>(short_offset & 0x7fffffffU) * 8,
                        8);
            if (!large)
                return std::unexpected(large.error());
            offset =
                (static_cast<std::uint64_t>(big_endian32(std::span{*large}.subspan(0, 4))) << 32U) |
                big_endian32(std::span{*large}.subspan(4, 4));
        }
        if (offset != wanted)
            continue;
        const auto value = read_at(
            index, names + static_cast<std::uint64_t>(number) * sha1{}.size(), sha1{}.size());
        if (!value)
            return std::unexpected(value.error());
        sha1 id{};
        std::ranges::copy(*value, id.begin());
        return std::optional<sha1>{id};
    }
    return std::optional<sha1>{};
}

auto pack_index_entries(const std::filesystem::path& index)
    -> result<std::vector<pack_index_entry>> {
    constexpr std::uint64_t cache_limit = 32U * 1024U * 1024U;
    const auto header = read_at(index, 0, 8 + 256 * 4);
    if (!header)
        return std::unexpected(header.error());
    if (big_endian32(std::span{*header}.subspan(0, 4)) != 0xff744f63U ||
        big_endian32(std::span{*header}.subspan(4, 4)) != 2U)
        return std::unexpected(
            failure(errc::corrupt, "unsupported Git pack index", index.string()));
    const auto count = big_endian32(std::span{*header}.subspan(8 + 255 * 4, 4));
    if (static_cast<std::uint64_t>(count) * sizeof(pack_index_entry) > cache_limit)
        return std::vector<pack_index_entry>{};
    constexpr std::uint64_t names = 8 + 256 * 4;
    const auto offset_table = names + static_cast<std::uint64_t>(count) * sha1{}.size() +
                              static_cast<std::uint64_t>(count) * 4;
    auto entries = std::vector<pack_index_entry>{};
    entries.reserve(count);
    for (std::uint32_t number{}; number < count; ++number) {
        const auto raw = read_at(index, offset_table + static_cast<std::uint64_t>(number) * 4, 4);
        if (!raw)
            return std::unexpected(raw.error());
        const auto short_offset = big_endian32(*raw);
        std::uint64_t offset = short_offset;
        if ((short_offset & 0x80000000U) != 0) {
            const auto large =
                read_at(index,
                        offset_table + static_cast<std::uint64_t>(count) * 4 +
                            static_cast<std::uint64_t>(short_offset & 0x7fffffffU) * 8,
                        8);
            if (!large)
                return std::unexpected(large.error());
            offset =
                (static_cast<std::uint64_t>(big_endian32(std::span{*large}.subspan(0, 4))) << 32U) |
                big_endian32(std::span{*large}.subspan(4, 4));
        }
        const auto value = read_at(
            index, names + static_cast<std::uint64_t>(number) * sha1{}.size(), sha1{}.size());
        if (!value)
            return std::unexpected(value.error());
        sha1 id{};
        std::ranges::copy(*value, id.begin());
        entries.push_back({offset, id});
    }
    return entries;
}

auto pack_id_at_offset(const std::filesystem::path& index,
                       std::span<const pack_index_entry> entries,
                       std::uint64_t wanted) -> result<std::optional<sha1>> {
    const auto found = std::ranges::find(entries, wanted, &pack_index_entry::offset);
    if (found != entries.end())
        return std::optional<sha1>{found->id};
    return scan_pack_id_at_offset(index, wanted);
}

auto cached_pack_offset(std::span<const pack_index_entry> entries, const sha1& id)
    -> std::optional<std::uint64_t> {
    const auto found = std::ranges::lower_bound(entries, id, {}, &pack_index_entry::id);
    if (found == entries.end() || found->id != id)
        return {};
    return found->offset;
}

auto delta_size(std::span<const std::byte> input, std::size_t& offset) -> result<std::uint64_t> {
    std::uint64_t value{};
    unsigned shift{};
    for (;;) {
        if (offset == input.size() || shift >= 64)
            return std::unexpected(failure(errc::corrupt, "invalid Git delta header"));
        const auto byte = std::to_integer<unsigned char>(input[offset++]);
        if (static_cast<std::uint64_t>(byte & 0x7fU) >
            (std::numeric_limits<std::uint64_t>::max() >> shift))
            return std::unexpected(failure(errc::corrupt, "invalid Git delta header"));
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0)
            return value;
        shift += 7;
    }
}

// A missing expected size denotes a loose object: read its bounded header before its body.
auto inflate_object(std::istream& input,
                    const std::filesystem::path& path,
                    std::uint64_t offset,
                    std::optional<std::uint64_t> expected_size,
                    decode_budget& budget,
                    bool delta = false) -> result<std::vector<std::byte>> {
    input.clear();
    input.seekg(static_cast<std::streamoff>(offset));
    auto stream = z_stream{};
    if (inflateInit(&stream) != Z_OK)
        return std::unexpected(failure(errc::internal, "cannot initialize zlib", path.string()));
    struct end_stream {
        z_stream& stream;
        ~end_stream() {
            inflateEnd(&stream);
        }
    } cleanup{stream};
    std::array<std::byte, 64 * 1024> input_block{};
    std::array<std::byte, 64 * 1024> output_block{};
    std::vector<std::byte> output;
    auto maximum = expected_size.value_or(64);
    bool header_pending = !expected_size;
    bool target_pending = delta && !budget.target;
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            input.read(reinterpret_cast<char*>(input_block.data()),
                       static_cast<std::streamsize>(input_block.size()));
            if (input.gcount() <= 0)
                return std::unexpected(
                    failure(errc::corrupt, "truncated compressed Git object", path.string()));
            stream.next_in = reinterpret_cast<Bytef*>(input_block.data());
            stream.avail_in = static_cast<uInt>(input.gcount());
        }
        const auto capacity =
            header_pending || target_pending
                ? 1ULL
                : std::min<std::uint64_t>(output_block.size(), maximum - output.size() + 1);
        stream.next_out = reinterpret_cast<Bytef*>(output_block.data());
        stream.avail_out = static_cast<uInt>(capacity);
        status = ::inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END)
            return std::unexpected(
                failure(errc::corrupt, "cannot inflate Git object", path.string()));
        const auto count = capacity - stream.avail_out;
        // Allow fixed-Huffman expansion while bounding streams of empty compressed blocks.
        if (count > maximum - output.size() || stream.total_in > maximum + maximum / 4 + 64 * 1024)
            return std::unexpected(
                failure(errc::corrupt, "Git object inflation limit exceeded", path.string()));
        if (const auto charged = budget.consume(count); !charged)
            return std::unexpected(charged.error());
        output.insert(output.end(), output_block.begin(), output_block.begin() + count);
        if (header_pending) {
            if (!output.empty() && output.back() == std::byte{}) {
                const auto header = std::string_view{reinterpret_cast<const char*>(output.data()),
                                                     output.size() - 1};
                const auto space = header.find(' ');
                std::uint64_t size{};
                if (space == std::string_view::npos || !parse_kind(header.substr(0, space)))
                    return std::unexpected(failure(errc::corrupt, "invalid Git object header"));
                const auto [end, code] =
                    std::from_chars(header.data() + space + 1, header.data() + header.size(), size);
                if (code != std::errc{} || end != header.data() + header.size() ||
                    size > budget.limits.object_bytes)
                    return std::unexpected(
                        failure(errc::corrupt, "Git object size limit exceeded"));
                maximum = output.size() + size;
                expected_size = maximum;
                header_pending = false;
                if (const auto target = budget.set_target(size); !target)
                    return std::unexpected(target.error());
            } else if (output.size() >= 64) {
                return std::unexpected(failure(errc::corrupt, "invalid Git object header"));
            }
        } else if (target_pending) {
            std::size_t cursor{};
            const auto source = delta_size(output, cursor);
            const auto target = delta_size(output, cursor);
            if (source && target) {
                if (*target > budget.limits.object_bytes)
                    return std::unexpected(
                        failure(errc::corrupt, "Git delta result limit exceeded"));
                if (const auto sized = budget.set_target(*target); !sized)
                    return std::unexpected(sized.error());
                target_pending = false;
            } else if (output.size() >= 20) {
                return std::unexpected(failure(errc::corrupt, "invalid Git delta header"));
            }
        }
    }
    if (header_pending || target_pending || !expected_size || output.size() != *expected_size)
        return std::unexpected(failure(errc::corrupt, "Git object size mismatch", path.string()));
    return output;
}

auto apply_delta(std::span<const std::byte> base,
                 std::span<const std::byte> delta,
                 decode_budget& budget) -> result<std::vector<std::byte>> {
    std::size_t offset{};
    const auto source_size = delta_size(delta, offset);
    const auto result_size = delta_size(delta, offset);
    // Each instruction byte yields at most 0x10000 bytes (a copy), so a declared result larger than
    // that cannot be produced and is refused before it is charged to the budget (liquid #407).
    constexpr auto largest_copy = std::uint64_t{0x10000};
    if (!source_size || !result_size || *source_size != base.size() ||
        *result_size > std::numeric_limits<std::size_t>::max() ||
        *result_size / largest_copy > delta.size())
        return std::unexpected(failure(errc::corrupt, "invalid Git delta sizes"));
    if (*result_size > budget.limits.object_bytes)
        return std::unexpected(failure(errc::corrupt, "Git delta result limit exceeded"));
    if (const auto charged = budget.consume(*result_size); !charged)
        return std::unexpected(charged.error());
    std::vector<std::byte> output;
    while (offset < delta.size()) {
        const auto instruction = std::to_integer<unsigned char>(delta[offset++]);
        if ((instruction & 0x80U) == 0) {
            if (instruction == 0 || instruction > delta.size() - offset ||
                instruction > *result_size - output.size())
                return std::unexpected(failure(errc::corrupt, "invalid Git delta insertion"));
            output.insert(
                output.end(), delta.begin() + offset, delta.begin() + offset + instruction);
            offset += instruction;
            continue;
        }
        std::uint32_t source_offset{};
        std::uint32_t size{};
        for (unsigned bit = 0; bit < 4; ++bit)
            if ((instruction & (1U << bit)) != 0) {
                if (offset == delta.size())
                    return std::unexpected(failure(errc::corrupt, "truncated Git delta copy"));
                source_offset |=
                    static_cast<std::uint32_t>(std::to_integer<unsigned char>(delta[offset++]))
                    << (bit * 8U);
            }
        for (unsigned bit = 0; bit < 3; ++bit)
            if ((instruction & (1U << (bit + 4U))) != 0) {
                if (offset == delta.size())
                    return std::unexpected(failure(errc::corrupt, "truncated Git delta copy"));
                size |= static_cast<std::uint32_t>(std::to_integer<unsigned char>(delta[offset++]))
                        << (bit * 8U);
            }
        if (size == 0)
            size = 0x10000U;
        if (source_offset > base.size() || size > base.size() - source_offset ||
            size > *result_size - output.size())
            return std::unexpected(failure(errc::corrupt, "Git delta copy exceeds base"));
        output.insert(
            output.end(), base.begin() + source_offset, base.begin() + source_offset + size);
    }
    if (output.size() != *result_size)
        return std::unexpected(failure(errc::corrupt, "Git delta result size mismatch"));
    return output;
}

} // namespace

auto parse_object(std::span<const std::byte> bytes, const sha1& id) -> result<object> {
    const auto zero = std::ranges::find(bytes, std::byte{});
    if (zero == bytes.end())
        return std::unexpected(
            failure(errc::corrupt, "Git object header is missing", sha1_hex(id)));
    const auto header = std::string_view{reinterpret_cast<const char*>(bytes.data()),
                                         static_cast<std::size_t>(zero - bytes.begin())};
    const auto separator = header.find(' ');
    if (separator == std::string_view::npos)
        return std::unexpected(
            failure(errc::corrupt, "Git object header is malformed", sha1_hex(id)));
    const auto kind = parse_kind(header.substr(0, separator));
    std::uint64_t size{};
    const auto [end, code] =
        std::from_chars(header.data() + separator + 1, header.data() + header.size(), size);
    if (!kind || code != std::errc{} || end != header.data() + header.size() ||
        size != static_cast<std::uint64_t>(bytes.end() - zero - 1))
        return std::unexpected(
            failure(errc::corrupt, "Git object header is invalid", sha1_hex(id)));
    if (sha1_hash(bytes) != id)
        return std::unexpected(failure(errc::corrupt, "Git object SHA-1 mismatch", sha1_hex(id)));
    return object{*kind, id, {zero + 1, bytes.end()}};
}

auto apply_delta(std::span<const std::byte> base, std::span<const std::byte> delta)
    -> result<std::vector<std::byte>> {
    // A delta applied on its own is held to the default read limits with a fresh budget.
    auto budget = decode_budget{.limits = object_read_limits{}};
    return apply_delta(base, delta, budget);
}

auto kind_name(object_kind kind) -> std::string_view {
    switch (kind) {
    case object_kind::blob:
        return "blob";
    case object_kind::tree:
        return "tree";
    case object_kind::commit:
        return "commit";
    case object_kind::tag:
        return "tag";
    }
    return "unknown";
}

object_store::object_store(std::filesystem::path directory, object_read_limits limits)
    : directory_(std::move(directory)), limits_(limits) {
    const object_read_limits defaults;
    limits_.object_bytes = std::min(limits_.object_bytes, defaults.object_bytes);
    limits_.read_slack_bytes = std::min(limits_.read_slack_bytes, defaults.read_slack_bytes);
    limits_.delta_depth = std::min(limits_.delta_depth, defaults.delta_depth);
}

auto object_store::stream_for(const std::filesystem::path& path) -> result<std::ifstream*> {
    const auto found = std::ranges::find(streams_, path, &pack_stream::path);
    if (found != streams_.end()) {
        auto stream = std::move(*found);
        streams_.erase(found);
        streams_.insert(streams_.begin(), std::move(stream));
        return streams_.front().input.get();
    }
    auto input = std::make_unique<std::ifstream>(path, std::ios::binary);
    if (!*input)
        return std::unexpected(failure(errc::not_found, "Git pack is not present", path.string()));
    streams_.insert(streams_.begin(), {path, std::move(input)});
    constexpr std::size_t stream_limit = 8;
    if (streams_.size() > stream_limit)
        streams_.pop_back();
    return streams_.front().input.get();
}

// Resolves delta chains iteratively: each link's program is inflated and kept until the base is
// found, then applied outward, so chain length never reaches the native stack.
auto object_store::read(const sha1& id) -> result<object> {
    auto cache = [this](const object& value) {
        constexpr std::uint64_t delta_cache_limit = 32U * 1024U * 1024U;
        const auto name = sha1_hex(value.id);
        if (value.payload.size() > delta_cache_limit)
            return;
        if (const auto found = delta_cache_.find(name); found != delta_cache_.end()) {
            delta_lru_.splice(delta_lru_.begin(), delta_lru_, found->second.position);
            return;
        }
        while (delta_cache_bytes_ + value.payload.size() > delta_cache_limit) {
            const auto oldest = delta_cache_.find(delta_lru_.back());
            delta_cache_bytes_ -= oldest->second.value.payload.size();
            delta_cache_.erase(oldest);
            delta_lru_.pop_back();
        }
        delta_cache_bytes_ += value.payload.size();
        delta_lru_.push_front(name);
        delta_cache_.emplace(name, cached_object{value, delta_lru_.begin()});
    };
    struct delta_link {
        std::vector<std::byte> program;
        sha1 expected;
    };
    decode_budget budget{.limits = limits_};
    std::vector<delta_link> chain;
    std::set<std::pair<std::filesystem::path, std::uint64_t>> visited;
    auto expected = id;
    // A name to find in the cache, loose storage or any pack; empty while following an offset.
    auto lookup = std::optional<sha1>{id};
    std::filesystem::path index;
    std::filesystem::path pack;
    std::uint64_t position{};
    object base{};
    for (;;) {
        if (chain.size() > limits_.delta_depth)
            return std::unexpected(failure(errc::corrupt, "Git delta depth limit exceeded"));
        if (lookup) {
            const auto hex = sha1_hex(*lookup);
            if (const auto cached = delta_cache_.find(hex); cached != delta_cache_.end()) {
                const auto size = cached->second.value.payload.size();
                if (const auto target = budget.set_target(size); !target)
                    return std::unexpected(target.error());
                if (const auto charged = budget.consume(size); !charged)
                    return std::unexpected(charged.error());
                delta_lru_.splice(delta_lru_.begin(), delta_lru_, cached->second.position);
                base = cached->second.value;
                break;
            }
            const auto loose_path = directory_ / "objects" / hex.substr(0, 2) / hex.substr(2);
            auto loose = std::ifstream{loose_path, std::ios::binary};
            if (loose) {
                const auto raw = inflate_object(loose, loose_path, 0, {}, budget);
                if (!raw)
                    return std::unexpected(raw.error());
                auto parsed = parse_object(*raw, *lookup);
                if (!parsed)
                    return std::unexpected(parsed.error());
                base = std::move(*parsed);
                break;
            }
            std::error_code code;
            const auto pack_directory = directory_ / "objects" / "pack";
            if (!std::filesystem::exists(pack_directory))
                return std::unexpected(failure(errc::not_found, "Git object is not present", hex));
            auto found = std::optional<std::uint64_t>{};
            for (std::filesystem::directory_iterator it{pack_directory, code}, end;
                 !code && it != end;
                 it.increment(code)) {
                if (it->path().extension() != ".idx")
                    continue;
                if (indexed_pack_ != it->path()) {
                    const auto entries = pack_index_entries(it->path());
                    if (!entries)
                        return std::unexpected(entries.error());
                    indexed_pack_ = it->path();
                    indexed_objects_ = *entries;
                }
                const auto offset = indexed_objects_.empty()
                                        ? pack_offset(it->path(), *lookup)
                                        : result<std::optional<std::uint64_t>>{
                                              cached_pack_offset(indexed_objects_, *lookup)};
                if (!offset)
                    return std::unexpected(offset.error());
                if (*offset) {
                    found = **offset;
                    index = it->path();
                    break;
                }
            }
            if (code)
                return std::unexpected(failure(errc::io, code.message(), pack_directory.string()));
            if (!found)
                return std::unexpected(failure(errc::not_found, "Git object is not present", hex));
            pack = index;
            pack.replace_extension(".pack");
            position = *found;
            expected = *lookup;
            lookup.reset();
        }
        if (!visited.emplace(pack, position).second)
            return std::unexpected(failure(errc::corrupt, "cyclic Git delta base"));
        const auto input = stream_for(pack);
        if (!input)
            return std::unexpected(input.error());
        const auto header = read_at(**input, pack, position, 64);
        if (!header)
            return std::unexpected(header.error());
        std::size_t cursor{};
        auto byte = std::to_integer<unsigned char>(header->at(cursor++));
        const auto type = (byte >> 4U) & 7U;
        std::uint64_t size = byte & 0x0fU;
        unsigned shift = 4;
        while ((byte & 0x80U) != 0) {
            if (cursor == header->size() || shift >= 64)
                return std::unexpected(
                    failure(errc::corrupt, "invalid packed Git header", pack.string()));
            byte = std::to_integer<unsigned char>(header->at(cursor++));
            if (static_cast<std::uint64_t>(byte & 0x7fU) >
                (std::numeric_limits<std::uint64_t>::max() >> shift))
                return std::unexpected(
                    failure(errc::corrupt, "invalid packed Git header", pack.string()));
            size |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
            shift += 7;
        }
        if (size > limits_.object_bytes)
            return std::unexpected(failure(errc::corrupt, "Git object size limit exceeded"));
        if (type < 1U || type > 7U || type == 5U)
            return std::unexpected(failure(errc::corrupt, "invalid packed Git object"));
        if (type <= 4U) {
            if (const auto target = budget.set_target(size); !target)
                return std::unexpected(target.error());
        }
        auto base_position = std::optional<std::uint64_t>{};
        auto reference = sha1{};
        if (type == 6U) {
            if (cursor == header->size())
                return std::unexpected(
                    failure(errc::corrupt, "truncated OFS_DELTA", pack.string()));
            byte = std::to_integer<unsigned char>(header->at(cursor++));
            std::uint64_t distance = byte & 0x7fU;
            while ((byte & 0x80U) != 0) {
                if (cursor == header->size() ||
                    distance >= (std::numeric_limits<std::uint64_t>::max() >> 7U))
                    return std::unexpected(
                        failure(errc::corrupt, "truncated OFS_DELTA", pack.string()));
                byte = std::to_integer<unsigned char>(header->at(cursor++));
                distance = ((distance + 1U) << 7U) | (byte & 0x7fU);
            }
            if (distance == 0 || distance > position)
                return std::unexpected(
                    failure(errc::corrupt, "invalid OFS_DELTA base", pack.string()));
            // The cached entries may belong to another pack after a reference lookup.
            const auto entries = indexed_pack_ == index
                                     ? std::span<const pack_index_entry>{indexed_objects_}
                                     : std::span<const pack_index_entry>{};
            const auto base_id = pack_id_at_offset(index, entries, position - distance);
            if (!base_id)
                return std::unexpected(base_id.error());
            if (!*base_id)
                return std::unexpected(
                    failure(errc::corrupt, "OFS_DELTA base is absent from index", pack.string()));
            base_position = position - distance;
            reference = **base_id;
        } else if (type == 7U) {
            if (cursor + reference.size() > header->size())
                return std::unexpected(
                    failure(errc::corrupt, "truncated REF_DELTA", pack.string()));
            std::ranges::copy_n(header->begin() + cursor, reference.size(), reference.begin());
            cursor += reference.size();
        }
        const auto inflated_input = stream_for(pack);
        if (!inflated_input)
            return std::unexpected(inflated_input.error());
        auto inflated = inflate_object(
            **inflated_input, pack, position + cursor, size, budget, type == 6U || type == 7U);
        if (!inflated)
            return std::unexpected(inflated.error());
        if (type <= 4U) {
            const auto kind = type == 1U   ? object_kind::commit
                              : type == 2U ? object_kind::tree
                              : type == 3U ? object_kind::blob
                                           : object_kind::tag;
            base = {kind, expected, std::move(*inflated)};
            if (sha1_hash(object_bytes(base.kind, base.payload)) != expected)
                return std::unexpected(
                    failure(errc::corrupt, "Git object SHA-1 mismatch", sha1_hex(expected)));
            cache(base);
            break;
        }
        chain.push_back({std::move(*inflated), expected});
        if (base_position) {
            position = *base_position;
            expected = reference;
        } else {
            lookup = reference;
        }
    }
    for (auto link = chain.rbegin(); link != chain.rend(); ++link) {
        auto resolved = apply_delta(base.payload, link->program, budget);
        if (!resolved)
            return std::unexpected(resolved.error());
        link->program = {};
        base = {base.kind, link->expected, std::move(*resolved)};
        if (sha1_hash(object_bytes(base.kind, base.payload)) != link->expected)
            return std::unexpected(
                failure(errc::corrupt, "Git object SHA-1 mismatch", sha1_hex(link->expected)));
        cache(base);
    }
    return base;
}

auto object_store::read_commit(const sha1& id) -> result<commit> {
    const auto object = read(id);
    if (!object)
        return std::unexpected(object.error());
    return parse_commit(*object);
}

auto parse_commit(const object& input) -> result<commit> {
    const auto& id = input.id;
    if (input.kind != object_kind::commit)
        return std::unexpected(failure(errc::corrupt, "Git object is not a commit", sha1_hex(id)));
    const auto content =
        std::string{reinterpret_cast<const char*>(input.payload.data()), input.payload.size()};
    const auto body = content.find("\n\n");
    if (body == std::string::npos)
        return std::unexpected(
            failure(errc::corrupt, "Git commit has no message separator", sha1_hex(id)));
    commit output{.id = id, .message = content.substr(body + 2)};
    std::size_t start{};
    while (start < body) {
        const auto end = content.find('\n', start);
        const auto line = std::string_view{content}.substr(start, end - start);
        if (line.starts_with("tree ")) {
            const auto parsed = parse_sha1(line.substr(5));
            if (!parsed)
                return std::unexpected(
                    failure(errc::corrupt, "invalid Git tree identifier", sha1_hex(id)));
            output.tree = *parsed;
        } else if (line.starts_with("parent ")) {
            const auto parsed = parse_sha1(line.substr(7));
            if (!parsed)
                return std::unexpected(
                    failure(errc::corrupt, "invalid Git parent identifier", sha1_hex(id)));
            output.parents.push_back(*parsed);
        } else if (line.starts_with("author ")) {
            const auto value = line.substr(7);
            const auto zone = value.rfind(' ');
            const auto seconds =
                zone == std::string_view::npos ? std::string_view{} : value.substr(0, zone);
            const auto time = seconds.rfind(' ');
            if (time == std::string_view::npos)
                return std::unexpected(failure(errc::corrupt, "invalid Git author", sha1_hex(id)));
            const auto [parsed, code] = std::from_chars(
                seconds.data() + time + 1, seconds.data() + seconds.size(), output.author_time);
            if (code != std::errc{} || parsed != seconds.data() + seconds.size())
                return std::unexpected(
                    failure(errc::corrupt, "invalid Git author time", sha1_hex(id)));
            output.author = std::string{seconds.substr(0, time)};
        }
        start = end + 1;
    }
    if (output.author.empty() || output.tree == sha1{})
        return std::unexpected(
            failure(errc::corrupt, "Git commit is missing required fields", sha1_hex(id)));
    return output;
}

auto object_store::read_tree(const sha1& id) -> result<std::vector<tree_entry>> {
    const auto object = read(id);
    if (!object)
        return std::unexpected(object.error());
    return parse_tree(*object);
}

auto parse_tree(const object& value) -> result<std::vector<tree_entry>> {
    const auto& id = value.id;
    const auto& payload = value.payload;
    if (value.kind != object_kind::tree)
        return std::unexpected(failure(errc::corrupt, "Git object is not a tree", sha1_hex(id)));
    std::vector<tree_entry> entries;
    for (std::size_t offset{}; offset < payload.size();) {
        const auto zero = std::find(payload.begin() + offset, payload.end(), std::byte{});
        if (zero == payload.end() || payload.end() - zero - 1 < 20)
            return std::unexpected(failure(errc::corrupt, "invalid Git tree entry", sha1_hex(id)));
        const auto header =
            std::string_view{reinterpret_cast<const char*>(payload.data() + offset),
                             static_cast<std::size_t>(zero - payload.begin() - offset)};
        const auto space = header.find(' ');
        // Checked before the parse: header.data() + npos is an out-of-bounds pointer (liquid #407).
        if (space == std::string_view::npos)
            return std::unexpected(failure(errc::corrupt, "invalid Git tree mode", sha1_hex(id)));
        std::uint32_t mode{};
        const auto [end, code] = std::from_chars(header.data(), header.data() + space, mode, 8);
        if (code != std::errc{} || end != header.data() + space)
            return std::unexpected(failure(errc::corrupt, "invalid Git tree mode", sha1_hex(id)));
        const auto name = header.substr(space + 1);
        if (const auto problem = objects::entry_name_problem(name))
            return std::unexpected(
                failure(errc::corrupt,
                        std::string{*problem},
                        sha1_hex(id) + " " + objects::printable_entry_name(name)));
        sha1 child{};
        std::ranges::copy_n(zero + 1, child.size(), child.begin());
        entries.push_back({std::string{name}, mode, child});
        offset = static_cast<std::size_t>(zero - payload.begin()) + 1 + child.size();
    }
    return entries;
}

auto object_store::peel(const sha1& id) -> result<sha1> {
    auto current = id;
    for (;;) {
        const auto object = read(current);
        if (!object)
            return std::unexpected(object.error());
        if (object->kind != object_kind::tag)
            return current;
        const auto content = std::string{reinterpret_cast<const char*>(object->payload.data()),
                                         object->payload.size()};
        const auto line_end = content.find('\n');
        const auto line = std::string_view{content}.substr(0, line_end);
        if (!line.starts_with("object "))
            return std::unexpected(failure(errc::corrupt, "invalid Git tag", sha1_hex(current)));
        const auto next = parse_sha1(line.substr(7));
        if (!next)
            return std::unexpected(
                failure(errc::corrupt, "invalid Git tag object", sha1_hex(current)));
        current = *next;
    }
}

auto object_store::refs() -> result<std::map<std::string, sha1>> {
    std::map<std::string, sha1> output;
    std::map<std::string, std::string> symbolic;
    const auto root = directory_ / "refs";
    std::error_code code;
    for (std::filesystem::recursive_directory_iterator it{root, code}, end; !code && it != end;
         it.increment(code)) {
        if (!it->is_regular_file())
            continue;
        const auto value = read_file(it->path());
        if (!value)
            return std::unexpected(value.error());
        const auto name = std::filesystem::relative(it->path(), directory_).generic_string();
        const auto text =
            trim(std::string{reinterpret_cast<const char*>(value->data()), value->size()});
        if (text.starts_with("ref: ")) {
            symbolic.emplace(name, text.substr(5));
            continue;
        }
        const auto parsed = parse_sha1(text);
        if (!parsed)
            return std::unexpected(failure(errc::corrupt, "invalid Git ref", it->path().string()));
        output.emplace(name, *parsed);
    }
    if (code)
        return std::unexpected(failure(errc::io, code.message(), root.string()));
    const auto packed = directory_ / "packed-refs";
    if (std::filesystem::exists(packed)) {
        auto input = std::ifstream{packed};
        std::string line;
        while (std::getline(input, line)) {
            if (line.empty() || line.front() == '#' || line.front() == '^')
                continue;
            const auto space = line.find(' ');
            const auto parsed =
                space == std::string::npos ? std::nullopt : parse_sha1(line.substr(0, space));
            if (!parsed)
                return std::unexpected(
                    failure(errc::corrupt, "invalid packed Git ref", packed.string()));
            output.try_emplace(line.substr(space + 1), *parsed);
        }
    }
    for (const auto& [name, target] : symbolic) {
        auto current = target;
        std::set<std::string> visited{name};
        for (;;) {
            if (!visited.insert(current).second)
                return std::unexpected(failure(errc::corrupt, "cyclic symbolic Git ref", name));
            if (const auto found = output.find(current); found != output.end()) {
                output.emplace(name, found->second);
                break;
            }
            const auto next = symbolic.find(current);
            if (next == symbolic.end())
                return std::unexpected(
                    failure(errc::corrupt, "symbolic Git ref target is missing", current));
            current = next->second;
        }
    }
    return output;
}

auto object_store::git_directory() const -> const std::filesystem::path& {
    return directory_;
}

auto git_directory(const std::filesystem::path& source) -> result<std::filesystem::path> {
    const auto directory =
        std::filesystem::is_directory(source / ".git") ? source / ".git" : source;
    if (!std::filesystem::is_directory(directory / "objects") ||
        !std::filesystem::exists(directory / "HEAD"))
        return std::unexpected(
            failure(errc::not_found, "Git repository is not present", source.string()));
    return directory;
}

auto write_loose_object(const std::filesystem::path& directory,
                        object_kind kind,
                        std::span<const std::byte> payload) -> result<sha1> {
    const auto raw = object_bytes(kind, payload);
    const auto id = sha1_hash(raw);
    const auto hex = sha1_hex(id);
    const auto path = directory / "objects" / hex.substr(0, 2) / hex.substr(2);
    std::error_code code;
    std::filesystem::create_directories(path.parent_path(), code);
    if (code)
        return std::unexpected(failure(errc::io, code.message(), path.parent_path().string()));
    if (std::filesystem::exists(path))
        return id;
    const auto compressed = deflate(raw, hex);
    if (!compressed)
        return std::unexpected(compressed.error());
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(compressed->data()),
                 static_cast<std::streamsize>(compressed->size()));
    if (!output)
        return std::unexpected(failure(errc::io, "cannot write Git object", path.string()));
    return id;
}

} // namespace liquid::git_detail
