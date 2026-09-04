#include "git/object_store.hpp"

#include <algorithm>
#include <charconv>
#include <fstream>
#include <iterator>
#include <limits>

#include <zlib.h>

namespace liquid::git_detail {
namespace {

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

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

auto inflate(std::span<const std::byte> input, std::string_view context)
    -> result<std::vector<std::byte>> {
    auto stream = z_stream{};
    stream.next_in = reinterpret_cast<Bytef*>(const_cast<std::byte*>(input.data()));
    stream.avail_in = static_cast<uInt>(input.size());
    if (inflateInit(&stream) != Z_OK)
        return std::unexpected(
            failure(errc::internal, "cannot initialize zlib", std::string{context}));
    std::vector<std::byte> output;
    std::array<std::byte, 64 * 1024> block{};
    int status{};
    do {
        stream.next_out = reinterpret_cast<Bytef*>(block.data());
        stream.avail_out = static_cast<uInt>(block.size());
        status = ::inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::unexpected(
                failure(errc::corrupt, "cannot inflate Git object", std::string{context}));
        }
        output.insert(
            output.end(), block.begin(), block.begin() + (block.size() - stream.avail_out));
    } while (status != Z_STREAM_END);
    inflateEnd(&stream);
    return output;
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

auto read_at(const std::filesystem::path& path, std::uint64_t offset, std::size_t size)
    -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        return std::unexpected(failure(errc::not_found, "Git pack is not present", path.string()));
    input.seekg(static_cast<std::streamoff>(offset));
    auto output = std::vector<std::byte>(size);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input)
        return std::unexpected(failure(errc::corrupt, "truncated Git pack", path.string()));
    return output;
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

struct inflated_pack {
    std::vector<std::byte> bytes;
};

auto inflate_pack(const std::filesystem::path& path, std::uint64_t offset)
    -> result<inflated_pack> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        return std::unexpected(failure(errc::not_found, "Git pack is not present", path.string()));
    input.seekg(static_cast<std::streamoff>(offset));
    auto stream = z_stream{};
    if (inflateInit(&stream) != Z_OK)
        return std::unexpected(failure(errc::internal, "cannot initialize zlib", path.string()));
    std::array<std::byte, 64 * 1024> input_block{};
    std::array<std::byte, 64 * 1024> output_block{};
    inflated_pack output{};
    int status = Z_OK;
    while (status != Z_STREAM_END) {
        if (stream.avail_in == 0) {
            input.read(reinterpret_cast<char*>(input_block.data()),
                       static_cast<std::streamsize>(input_block.size()));
            const auto count = input.gcount();
            if (count <= 0) {
                inflateEnd(&stream);
                return std::unexpected(
                    failure(errc::corrupt, "truncated compressed Git object", path.string()));
            }
            stream.next_in = reinterpret_cast<Bytef*>(input_block.data());
            stream.avail_in = static_cast<uInt>(count);
        }
        stream.next_out = reinterpret_cast<Bytef*>(output_block.data());
        stream.avail_out = static_cast<uInt>(output_block.size());
        status = ::inflate(&stream, Z_NO_FLUSH);
        if (status != Z_OK && status != Z_STREAM_END) {
            inflateEnd(&stream);
            return std::unexpected(
                failure(errc::corrupt, "cannot inflate packed Git object", path.string()));
        }
        output.bytes.insert(output.bytes.end(),
                            output_block.begin(),
                            output_block.begin() + (output_block.size() - stream.avail_out));
    }
    inflateEnd(&stream);
    return output;
}

auto delta_size(std::span<const std::byte> input, std::size_t& offset) -> result<std::uint64_t> {
    std::uint64_t value{};
    unsigned shift{};
    for (;;) {
        if (offset == input.size() || shift >= 64)
            return std::unexpected(failure(errc::corrupt, "invalid Git delta header"));
        const auto byte = std::to_integer<unsigned char>(input[offset++]);
        value |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
        if ((byte & 0x80U) == 0)
            return value;
        shift += 7;
    }
}

auto apply_delta(std::span<const std::byte> base, std::span<const std::byte> delta)
    -> result<std::vector<std::byte>> {
    std::size_t offset{};
    const auto source_size = delta_size(delta, offset);
    const auto result_size = delta_size(delta, offset);
    if (!source_size || !result_size || *source_size != base.size() ||
        *result_size > std::numeric_limits<std::size_t>::max())
        return std::unexpected(failure(errc::corrupt, "invalid Git delta sizes"));
    std::vector<std::byte> output;
    output.reserve(static_cast<std::size_t>(*result_size));
    while (offset < delta.size()) {
        const auto instruction = std::to_integer<unsigned char>(delta[offset++]);
        if ((instruction & 0x80U) == 0) {
            if (instruction == 0 || offset + instruction > delta.size())
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
        if (source_offset > base.size() || size > base.size() - source_offset)
            return std::unexpected(failure(errc::corrupt, "Git delta copy exceeds base"));
        output.insert(
            output.end(), base.begin() + source_offset, base.begin() + source_offset + size);
    }
    if (output.size() != *result_size)
        return std::unexpected(failure(errc::corrupt, "Git delta result size mismatch"));
    return output;
}

} // namespace

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

object_store::object_store(std::filesystem::path directory) : directory_(std::move(directory)) {}

auto object_store::read(const sha1& id) -> result<object> {
    const auto hex = sha1_hex(id);
    if (const auto cached = delta_cache_.find(hex); cached != delta_cache_.end())
        return cached->second;
    const auto compressed = read_file(directory_ / "objects" / hex.substr(0, 2) / hex.substr(2));
    if (compressed) {
        const auto raw = inflate(*compressed, hex);
        if (!raw)
            return std::unexpected(raw.error());
        return parse_object(*raw, id);
    }
    if (compressed.error().code != errc::not_found)
        return std::unexpected(compressed.error());

    std::error_code code;
    const auto pack_directory = directory_ / "objects" / "pack";
    if (!std::filesystem::exists(pack_directory))
        return std::unexpected(failure(errc::not_found, "Git object is not present", hex));
    for (std::filesystem::directory_iterator it{pack_directory, code}, end; !code && it != end;
         it.increment(code)) {
        if (it->path().extension() != ".idx")
            continue;
        const auto offset = pack_offset(it->path(), id);
        if (!offset)
            return std::unexpected(offset.error());
        if (!*offset)
            continue;
        auto pack = it->path();
        pack.replace_extension(".pack");
        auto decode = [&](auto&& self,
                          std::uint64_t position,
                          std::optional<sha1> expected) -> result<object> {
            const auto header = read_at(pack, position, 64);
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
                size |= static_cast<std::uint64_t>(byte & 0x7fU) << shift;
                shift += 7;
            }
            auto base = std::optional<object>{};
            if (type == 6U) {
                if (cursor == header->size())
                    return std::unexpected(
                        failure(errc::corrupt, "truncated OFS_DELTA", pack.string()));
                byte = std::to_integer<unsigned char>(header->at(cursor++));
                std::uint64_t distance = byte & 0x7fU;
                while ((byte & 0x80U) != 0) {
                    if (cursor == header->size())
                        return std::unexpected(
                            failure(errc::corrupt, "truncated OFS_DELTA", pack.string()));
                    byte = std::to_integer<unsigned char>(header->at(cursor++));
                    distance = ((distance + 1U) << 7U) | (byte & 0x7fU);
                }
                if (distance > position)
                    return std::unexpected(
                        failure(errc::corrupt, "invalid OFS_DELTA base", pack.string()));
                auto decoded = self(self, position - distance, std::nullopt);
                if (!decoded)
                    return std::unexpected(decoded.error());
                base = std::move(*decoded);
            } else if (type == 7U) {
                if (cursor + sha1{}.size() > header->size())
                    return std::unexpected(
                        failure(errc::corrupt, "truncated REF_DELTA", pack.string()));
                sha1 base_id{};
                std::ranges::copy_n(header->begin() + cursor, base_id.size(), base_id.begin());
                cursor += base_id.size();
                auto decoded = read(base_id);
                if (!decoded)
                    return std::unexpected(decoded.error());
                base = std::move(*decoded);
            }
            const auto inflated = inflate_pack(pack, position + cursor);
            if (!inflated)
                return std::unexpected(inflated.error());
            object output{};
            if (type == 6U || type == 7U) {
                const auto resolved = apply_delta(base->payload, inflated->bytes);
                if (!resolved)
                    return std::unexpected(resolved.error());
                if (resolved->size() != size)
                    return std::unexpected(
                        failure(errc::corrupt, "packed Git delta size mismatch", pack.string()));
                output = {base->kind, expected.value_or(sha1{}), *resolved};
            } else {
                const auto kind = type == 1U   ? object_kind::commit
                                  : type == 2U ? object_kind::tree
                                  : type == 3U ? object_kind::blob
                                  : type == 4U ? object_kind::tag
                                               : object_kind{};
                if (type < 1U || type > 4U || inflated->bytes.size() != size)
                    return std::unexpected(
                        failure(errc::corrupt, "invalid packed Git object", pack.string()));
                output = {kind, expected.value_or(sha1{}), std::move(inflated->bytes)};
            }
            if (expected && sha1_hash(object_bytes(output.kind, output.payload)) != *expected)
                return std::unexpected(
                    failure(errc::corrupt, "Git object SHA-1 mismatch", sha1_hex(*expected)));
            return output;
        };
        auto object = decode(decode, **offset, id);
        if (!object)
            return std::unexpected(object.error());
        if (object->payload.size() <= 32U * 1024U * 1024U) {
            if (delta_cache_bytes_ + object->payload.size() > 32U * 1024U * 1024U) {
                delta_cache_.clear();
                delta_cache_bytes_ = 0;
            }
            delta_cache_bytes_ += object->payload.size();
            delta_cache_.emplace(hex, *object);
        }
        return object;
    }
    if (code)
        return std::unexpected(failure(errc::io, code.message(), pack_directory.string()));
    return std::unexpected(failure(errc::not_found, "Git object is not present", hex));
}

auto object_store::read_commit(const sha1& id) -> result<commit> {
    const auto object = read(id);
    if (!object)
        return std::unexpected(object.error());
    if (object->kind != object_kind::commit)
        return std::unexpected(failure(errc::corrupt, "Git object is not a commit", sha1_hex(id)));
    const auto content =
        std::string{reinterpret_cast<const char*>(object->payload.data()), object->payload.size()};
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
    if (object->kind != object_kind::tree)
        return std::unexpected(failure(errc::corrupt, "Git object is not a tree", sha1_hex(id)));
    std::vector<tree_entry> entries;
    for (std::size_t offset{}; offset < object->payload.size();) {
        const auto zero =
            std::find(object->payload.begin() + offset, object->payload.end(), std::byte{});
        if (zero == object->payload.end() || object->payload.end() - zero - 1 < 20)
            return std::unexpected(failure(errc::corrupt, "invalid Git tree entry", sha1_hex(id)));
        const auto header =
            std::string_view{reinterpret_cast<const char*>(object->payload.data() + offset),
                             static_cast<std::size_t>(zero - object->payload.begin() - offset)};
        const auto space = header.find(' ');
        std::uint32_t mode{};
        const auto [end, code] = std::from_chars(header.data(), header.data() + space, mode, 8);
        if (space == std::string_view::npos || code != std::errc{} || end != header.data() + space)
            return std::unexpected(failure(errc::corrupt, "invalid Git tree mode", sha1_hex(id)));
        sha1 child{};
        std::ranges::copy_n(zero + 1, child.size(), child.begin());
        entries.push_back({std::string{header.substr(space + 1)}, mode, child});
        offset = static_cast<std::size_t>(zero - object->payload.begin()) + 1 + child.size();
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
    const auto root = directory_ / "refs";
    std::error_code code;
    for (std::filesystem::recursive_directory_iterator it{root, code}, end; !code && it != end;
         it.increment(code)) {
        if (!it->is_regular_file())
            continue;
        const auto value = read_file(it->path());
        if (!value)
            return std::unexpected(value.error());
        const auto parsed = parse_sha1(
            trim(std::string{reinterpret_cast<const char*>(value->data()), value->size()}));
        if (!parsed)
            return std::unexpected(failure(errc::corrupt, "invalid Git ref", it->path().string()));
        output.emplace(std::filesystem::relative(it->path(), directory_).generic_string(), *parsed);
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
