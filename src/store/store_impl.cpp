#include "store/store_impl.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#include "store/chunker.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::uint64_t default_pack_size = 64ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t minimum_pack_size = 16ULL * 1024ULL * 1024ULL;
constexpr std::uint64_t maximum_pack_size = 64ULL * 1024ULL * 1024ULL;
constexpr std::string_view marker_contents = "LIQUID_STORE\nversion=1\n";
constexpr std::array<std::byte, 8> manifest_magic{
    std::byte{0x4c}, std::byte{0x51}, std::byte{0x4d}, std::byte{0x4e},
    std::byte{0x46}, std::byte{0x53}, std::byte{0x54}, std::byte{0x31}};
constexpr std::array<std::byte, 8> descriptor_magic{
    std::byte{0x4c}, std::byte{0x51}, std::byte{0x4f}, std::byte{0x42},
    std::byte{0x4a}, std::byte{0x45}, std::byte{0x43}, std::byte{0x31}};

bool known_algorithm(algorithm value) noexcept {
    return value == algorithm::blake3 || value == algorithm::sha256;
}

template <class Integer>
void append_little_endian(std::vector<std::byte>& output, Integer value) {
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.push_back(std::byte{static_cast<unsigned char>((value >> (index * 8U)) & 0xffU)});
    }
}

template <class Integer>
bool take_little_endian(std::span<const std::byte> input, std::size_t& offset, Integer& value) {
    if (input.size() - std::min(input.size(), offset) < sizeof(Integer)) return false;
    value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(std::to_integer<unsigned char>(input[offset + index]))
                 << (index * 8U);
    }
    offset += sizeof(Integer);
    return true;
}

void append_digest(std::vector<std::byte>& output, const digest& id) {
    output.push_back(std::byte{static_cast<unsigned char>(id.alg)});
    output.insert(output.end(), id.bytes.begin(), id.bytes.end());
}

bool take_digest(std::span<const std::byte> input, std::size_t& offset, digest& id) {
    if (input.size() - std::min(input.size(), offset) < 33) return false;
    id.alg = static_cast<algorithm>(std::to_integer<unsigned char>(input[offset++]));
    if (!known_algorithm(id.alg)) return false;
    std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(offset), id.bytes.size(), id.bytes.begin());
    offset += id.bytes.size();
    return true;
}

std::vector<std::byte> encode_manifest(
    std::uint64_t size, const std::vector<store_impl::chunk_reference>& chunks) {
    std::vector<std::byte> output;
    output.reserve(24 + chunks.size() * 37);
    output.insert(output.end(), manifest_magic.begin(), manifest_magic.end());
    append_little_endian(output, size);
    append_little_endian(output, static_cast<std::uint64_t>(chunks.size()));
    for (const auto& reference : chunks) {
        append_digest(output, reference.id);
        append_little_endian(output, reference.length);
    }
    return output;
}

std::vector<std::byte> encode_descriptor(
    std::uint64_t manifest_size,
    const std::vector<store_impl::chunk_reference>& chunks) {
    std::vector<std::byte> output;
    output.reserve(20 + chunks.size() * 37);
    output.insert(output.end(), descriptor_magic.begin(), descriptor_magic.end());
    append_little_endian(output, manifest_size);
    append_little_endian(output, static_cast<std::uint32_t>(chunks.size()));
    for (const auto& reference : chunks) {
        append_digest(output, reference.id);
        append_little_endian(output, reference.length);
    }
    return output;
}

error malformed(const digest& object, std::string message) {
    return error{errc::corrupt, std::move(message), to_string(object)};
}

result<void> prepare_layout(const std::filesystem::path& root) {
    std::error_code filesystem_error;
    const bool exists = std::filesystem::exists(root, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(error{errc::io, filesystem_error.message(), root.string()});
    }
    if (!exists) {
        std::filesystem::create_directories(root, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(error{errc::io, filesystem_error.message(), root.string()});
        }
    }

    const auto marker = root / "LIQUID_STORE";
    if (std::filesystem::exists(marker, filesystem_error)) {
        std::ifstream input(marker, std::ios::binary);
        std::string contents(
            (std::istreambuf_iterator<char>(input)), std::istreambuf_iterator<char>());
        if (!input.eof() && input.fail()) {
            return std::unexpected(error{errc::io, "could not read store marker", marker.string()});
        }
        if (contents != marker_contents) {
            return std::unexpected(error{errc::unsupported, "unknown liquid store format", marker.string()});
        }
    } else {
        if (!std::filesystem::is_empty(root, filesystem_error)) {
            return std::unexpected(error{errc::invalid_argument, "store root is not empty", root.string()});
        }
        std::ofstream output(marker, std::ios::binary | std::ios::trunc);
        output.write(marker_contents.data(), static_cast<std::streamsize>(marker_contents.size()));
        output.flush();
        if (!output) {
            return std::unexpected(error{errc::io, "could not write store marker", marker.string()});
        }
    }

    for (const auto* name : {"packs", "index", "tmp"}) {
        std::filesystem::create_directories(root / name, filesystem_error);
        if (filesystem_error) {
            return std::unexpected(error{errc::io, filesystem_error.message(), (root / name).string()});
        }
    }
    return {};
}

}  // namespace

store_impl::store_impl(
    algorithm hash_algorithm, std::unique_ptr<chunk_index> index, pack_store packs)
    : hash_algorithm_(hash_algorithm), index_(std::move(index)), packs_(std::move(packs)) {}

result<std::unique_ptr<store_impl>> store_impl::open(
    const std::filesystem::path& root, const store_options& options) {
    if (!known_algorithm(options.hash_algorithm)) {
        return std::unexpected(error{errc::invalid_argument, "unknown digest algorithm", root.string()});
    }
    const auto pack_size = options.pack_size == 0 ? default_pack_size : options.pack_size;
    if (pack_size < minimum_pack_size || pack_size > maximum_pack_size) {
        return std::unexpected(error{
            errc::invalid_argument, "pack size must be between 16 MiB and 64 MiB", root.string()});
    }
    auto layout = prepare_layout(root);
    if (!layout) return std::unexpected(layout.error());
    auto index = chunk_index::open(root / "index", options.index_cache_size);
    if (!index) return std::unexpected(index.error());
    auto packs = pack_store::open(root / "packs", pack_size);
    if (!packs) return std::unexpected(packs.error());
    return std::unique_ptr<store_impl>(
        new store_impl(options.hash_algorithm, std::move(*index), std::move(*packs)));
}

result<std::vector<store_impl::chunk_reference>> store_impl::write_chunks(
    std::span<const std::byte> input) {
    std::vector<chunk_reference> references;
    const auto ranges = chunk(input);
    references.reserve(ranges.size());
    for (const auto& range : ranges) {
        const auto bytes = input.subspan(range.offset, range.length);
        auto id = hash(hash_algorithm_, bytes);
        if (!id) return std::unexpected(id.error());
        auto present = index_->contains_chunk(*id);
        if (!present) return std::unexpected(present.error());
        if (!*present) {
            auto location = packs_.append(*id, bytes);
            if (!location) return std::unexpected(location.error());
            auto indexed = index_->put_chunk(*id, *location);
            if (!indexed) return std::unexpected(indexed.error());
        }
        references.push_back(chunk_reference{*id, static_cast<std::uint32_t>(range.length)});
    }
    return references;
}

result<digest> store_impl::put(std::span<const std::byte> input) {
    auto object = hash(hash_algorithm_, input);
    if (!object) return std::unexpected(object.error());
    auto present = index_->contains_object(*object);
    if (!present) return std::unexpected(present.error());
    if (*present) return *object;

    auto content_chunks = write_chunks(input);
    if (!content_chunks) return std::unexpected(content_chunks.error());
    const auto manifest = encode_manifest(input.size(), *content_chunks);
    auto manifest_chunks = write_chunks(manifest);
    if (!manifest_chunks) return std::unexpected(manifest_chunks.error());
    if (manifest_chunks->size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error{errc::internal, "object descriptor is too large", to_string(*object)});
    }
    const auto descriptor = encode_descriptor(manifest.size(), *manifest_chunks);
    auto indexed = index_->put_object(*object, descriptor);
    if (!indexed) return std::unexpected(indexed.error());
    return *object;
}

result<std::vector<std::byte>> store_impl::read_chunk(
    const chunk_reference& reference) const {
    auto location = index_->find_chunk(reference.id);
    if (!location) return std::unexpected(location.error());
    auto bytes = packs_.read(reference.id, *location);
    if (!bytes) return std::unexpected(bytes.error());
    if (bytes->size() != reference.length) {
        return std::unexpected(error{
            errc::corrupt, "chunk length does not match its manifest", to_string(reference.id)});
    }
    return bytes;
}

result<store_impl::object_manifest> store_impl::load_manifest(const digest& object) const {
    auto descriptor = index_->find_object(object);
    if (!descriptor) return std::unexpected(descriptor.error());
    const auto bytes = std::span<const std::byte>(*descriptor);
    if (bytes.size() < 20 || !std::equal(descriptor_magic.begin(), descriptor_magic.end(), bytes.begin())) {
        return std::unexpected(malformed(object, "object descriptor header is invalid"));
    }
    std::size_t offset = descriptor_magic.size();
    std::uint64_t manifest_size = 0;
    std::uint32_t chunk_count = 0;
    if (!take_little_endian(bytes, offset, manifest_size) ||
        !take_little_endian(bytes, offset, chunk_count) ||
        bytes.size() - offset != static_cast<std::size_t>(chunk_count) * 37) {
        return std::unexpected(malformed(object, "object descriptor is malformed"));
    }

    std::vector<std::byte> encoded_manifest;
    if (manifest_size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(malformed(object, "object manifest is too large for this platform"));
    }
    encoded_manifest.reserve(static_cast<std::size_t>(manifest_size));
    for (std::uint32_t index = 0; index < chunk_count; ++index) {
        chunk_reference reference{};
        if (!take_digest(bytes, offset, reference.id) ||
            !take_little_endian(bytes, offset, reference.length)) {
            return std::unexpected(malformed(object, "object descriptor chunk is malformed"));
        }
        auto chunk_bytes = read_chunk(reference);
        if (!chunk_bytes) return std::unexpected(chunk_bytes.error());
        encoded_manifest.insert(encoded_manifest.end(), chunk_bytes->begin(), chunk_bytes->end());
    }
    if (encoded_manifest.size() != manifest_size || encoded_manifest.size() < 24 ||
        !std::equal(manifest_magic.begin(), manifest_magic.end(), encoded_manifest.begin())) {
        return std::unexpected(malformed(object, "object manifest header is invalid"));
    }

    const auto manifest_bytes = std::span<const std::byte>(encoded_manifest);
    offset = manifest_magic.size();
    object_manifest manifest{};
    std::uint64_t content_chunk_count = 0;
    if (!take_little_endian(manifest_bytes, offset, manifest.size) ||
        !take_little_endian(manifest_bytes, offset, content_chunk_count) ||
        content_chunk_count > std::numeric_limits<std::size_t>::max() ||
        manifest_bytes.size() - offset != static_cast<std::size_t>(content_chunk_count) * 37) {
        return std::unexpected(malformed(object, "object manifest is malformed"));
    }
    manifest.chunks.reserve(static_cast<std::size_t>(content_chunk_count));
    std::uint64_t calculated_size = 0;
    for (std::uint64_t index = 0; index < content_chunk_count; ++index) {
        chunk_reference reference{};
        if (!take_digest(manifest_bytes, offset, reference.id) ||
            !take_little_endian(manifest_bytes, offset, reference.length) ||
            calculated_size > std::numeric_limits<std::uint64_t>::max() - reference.length) {
            return std::unexpected(malformed(object, "object manifest chunk is malformed"));
        }
        calculated_size += reference.length;
        manifest.chunks.push_back(reference);
    }
    if (calculated_size != manifest.size) {
        return std::unexpected(malformed(object, "object manifest size does not match its chunks"));
    }
    return manifest;
}

result<std::vector<std::byte>> store_impl::get(const digest& id) {
    auto manifest = load_manifest(id);
    if (!manifest) return std::unexpected(manifest.error());
    if (manifest->size > std::numeric_limits<std::size_t>::max()) {
        return std::unexpected(error{errc::invalid_argument, "object is too large for this platform", to_string(id)});
    }
    std::vector<std::byte> output;
    output.reserve(static_cast<std::size_t>(manifest->size));
    for (const auto& reference : manifest->chunks) {
        auto bytes = read_chunk(reference);
        if (!bytes) return std::unexpected(bytes.error());
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    return output;
}

result<std::uint64_t> store_impl::read(
    const digest& id, std::uint64_t offset, std::span<std::byte> into) {
    auto manifest = load_manifest(id);
    if (!manifest) return std::unexpected(manifest.error());
    if (offset >= manifest->size || into.empty()) return std::uint64_t{0};

    const auto wanted = std::min<std::uint64_t>(into.size(), manifest->size - offset);
    std::uint64_t object_offset = 0;
    std::uint64_t written = 0;
    for (const auto& reference : manifest->chunks) {
        const auto chunk_end = object_offset + reference.length;
        if (chunk_end <= offset) {
            object_offset = chunk_end;
            continue;
        }
        if (object_offset >= offset + wanted) break;
        auto bytes = read_chunk(reference);
        if (!bytes) return std::unexpected(bytes.error());
        const auto begin = static_cast<std::size_t>(offset > object_offset ? offset - object_offset : 0);
        const auto available = reference.length - begin;
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(available, wanted - written));
        std::copy_n(
            bytes->begin() + static_cast<std::ptrdiff_t>(begin), count,
            into.begin() + static_cast<std::ptrdiff_t>(written));
        written += count;
        object_offset = chunk_end;
    }
    return written;
}

result<bool> store_impl::contains(const digest& id) const {
    if (!known_algorithm(id.alg)) {
        return std::unexpected(error{errc::invalid_argument, "unknown digest algorithm", to_string(id)});
    }
    return index_->contains_object(id);
}

}  // namespace liquid::store_detail
