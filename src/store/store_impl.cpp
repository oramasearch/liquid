#include "store_impl.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <limits>
#include <numeric>
#include <set>
#include <stdexcept>
#include <string_view>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/this_coro.hpp>

#include "../runtime/net.hpp"
#include "blake3.h"
#include "chunker.hpp"
#include "collector.hpp"
#include "faults.hpp"
#include "hash/sha256.h"
#include "identity/key_version.hpp"
#include "mapped_file.hpp"
#include "objects/records.hpp"
#include "references.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> objects_magic_v1{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'O'},
                                                    std::byte{'B'},
                                                    std::byte{'J'},
                                                    std::byte{1},
                                                    std::byte{0},
                                                    std::byte{0}};
constexpr std::array<std::byte, 8> objects_magic_v2{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'O'},
                                                    std::byte{'B'},
                                                    std::byte{'J'},
                                                    std::byte{2},
                                                    std::byte{0},
                                                    std::byte{0}};
constexpr std::array<std::byte, 8> objects_magic_v3{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'O'},
                                                    std::byte{'B'},
                                                    std::byte{'J'},
                                                    std::byte{3},
                                                    std::byte{0},
                                                    std::byte{0}};
constexpr std::array<std::byte, 8> objects_magic_v4{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'O'},
                                                    std::byte{'B'},
                                                    std::byte{'J'},
                                                    std::byte{4},
                                                    std::byte{0},
                                                    std::byte{0}};
constexpr std::array<std::byte, 8> remotes_magic_v1{std::byte{'L'},
                                                    std::byte{'Q'},
                                                    std::byte{'R'},
                                                    std::byte{'E'},
                                                    std::byte{'M'},
                                                    std::byte{1},
                                                    std::byte{0},
                                                    std::byte{0}};

constexpr std::size_t file_window_size = 8U * 1024U * 1024U;

class incremental_digest {
public:
    incremental_digest(algorithm alg, const std::optional<crypto_detail::key>& key) {
        if (key) {
            algorithm_ = algorithm::blake3_keyed;
            blake3_hasher_init_keyed(&blake3_, reinterpret_cast<const std::uint8_t*>(key->data()));
            return;
        }
        algorithm_ = alg;
        if (algorithm_ == algorithm::blake3) {
            blake3_hasher_init(&blake3_);
            return;
        }
        sha256_init(&sha256_);
    }

    void update(std::span<const std::byte> input) {
        if (algorithm_ == algorithm::blake3 || algorithm_ == algorithm::blake3_keyed) {
            blake3_hasher_update(&blake3_, input.data(), input.size());
            return;
        }
        sha256_update(&sha256_, reinterpret_cast<const BYTE*>(input.data()), input.size());
    }

    [[nodiscard]] digest finish() {
        digest output{algorithm_, {}};
        if (algorithm_ == algorithm::blake3 || algorithm_ == algorithm::blake3_keyed) {
            blake3_hasher_finalize(&blake3_,
                                   reinterpret_cast<std::uint8_t*>(output.bytes.data()),
                                   output.bytes.size());
            return output;
        }
        sha256_final(&sha256_, reinterpret_cast<BYTE*>(output.bytes.data()));
        return output;
    }

private:
    algorithm algorithm_{algorithm::blake3};
    blake3_hasher blake3_{};
    SHA256_CTX sha256_{};
};

error store_error(errc code, std::string message, std::string context) {
    return {code, std::move(message), std::move(context)};
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    for (unsigned index = 0; index != 2; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}
void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}
void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}
std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 2 > bytes.size())
        return 0;
    return std::uint16_t(std::to_integer<unsigned char>(bytes[at])) |
           (std::uint16_t(std::to_integer<unsigned char>(bytes[at + 1])) << 8U);
}
std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 4 > bytes.size())
        return 0;
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index)
        value |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + index])) << (8U * index);
    return value;
}
std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 8 > bytes.size())
        return 0;
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + index])) << (8U * index);
    return value;
}
bool valid_algorithm(unsigned char value) {
    return value == static_cast<unsigned char>(algorithm::blake3) ||
           value == static_cast<unsigned char>(algorithm::sha256) ||
           value == static_cast<unsigned char>(algorithm::blake3_keyed);
}

} // namespace

store_backend::store_backend(std::filesystem::path root, store_options options)
    : root_(std::move(root)), options_(std::move(options)),
      remote_sources_(options_.remote_sources), index_(root_ / "index") {}

runtime::task<std::unique_ptr<store_backend>> store_backend::open(std::filesystem::path root,
                                                                  store_options options) {
    try {
        if (options.hash_algorithm != algorithm::blake3 &&
            options.hash_algorithm != algorithm::sha256) {
            co_return std::unexpected(
                store_error(errc::invalid_argument, "unknown hash algorithm", root.string()));
        }
        std::filesystem::create_directories(root / "packs");
        std::filesystem::create_directories(root / "index");
        std::filesystem::create_directories(root / "leases");
        std::filesystem::create_directories(root / "gc");
        std::filesystem::create_directories(root / "tmp");
        std::error_code ignored;
        std::filesystem::remove(root / "gc" / "MANIFEST.tmp", ignored);
        std::filesystem::remove(root / "packs" / "objects.meta.tmp", ignored);
        std::filesystem::remove(root / "packs" / "remotes.meta.tmp", ignored);
        const auto marker = root / "LIQUID_STORE";
        if (std::filesystem::exists(marker)) {
            auto text = co_await read_file(marker);
            if (!text)
                co_return std::unexpected(text.error());
            const std::string expected = "liquid-store 1\n";
            if (text->size() != expected.size() ||
                std::memcmp(text->data(), expected.data(), expected.size()) != 0) {
                co_return std::unexpected(
                    store_error(errc::unsupported, "unknown liquid store format", marker.string()));
            }
        } else {
            const std::string text = "liquid-store 1\n";
            std::vector<std::byte> bytes(
                reinterpret_cast<const std::byte*>(text.data()),
                reinterpret_cast<const std::byte*>(text.data() + text.size()));
            auto saved = co_await write_file(marker, std::move(bytes));
            if (!saved)
                co_return std::unexpected(saved.error());
        }
        auto result =
            std::unique_ptr<store_backend>(new store_backend(std::move(root), std::move(options)));
        if (result->options_.keys) {
            auto attached = co_await result->options_.keys->attach(result->root_ / "keys");
            if (!attached)
                co_return std::unexpected(attached.error());
        }
        auto objects = co_await result->load_objects();
        if (!objects)
            co_return std::unexpected(objects.error());
        auto remotes = co_await result->load_remotes();
        if (!remotes)
            co_return std::unexpected(remotes.error());
        auto collection = co_await load_collection_manifest(result->root_);
        if (!collection)
            co_return std::unexpected(collection.error());
        auto recovered = co_await result->recover_pending_compactions(*collection);
        if (!recovered)
            co_return std::unexpected(recovered.error());
        const bool manifest_exists = std::filesystem::exists(result->root_ / "index" / "MANIFEST");
        auto opened = co_await result->index_.open();
        if (!opened && opened.error().code != errc::corrupt)
            co_return std::unexpected(opened.error());
        auto indexed = co_await result->index_packs(
            !opened || !manifest_exists || result->index_.collection_epoch() != collection->epoch);
        if (!indexed)
            co_return std::unexpected(indexed.error());
        auto subjects = co_await result->rebuild_subject_index();
        if (!subjects)
            co_return std::unexpected(subjects.error());
        auto remote_records = co_await result->rebuild_remote_record_index();
        if (!remote_records)
            co_return std::unexpected(remote_records.error());
        co_return result;
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), root.string()));
    } catch (const std::exception& caught) {
        co_return std::unexpected(store_error(errc::internal, caught.what(), root.string()));
    }
}

runtime::task<void>
store_backend::recover_pending_compactions(const collection_manifest& manifest) {
    try {
        for (const auto& entry : std::filesystem::directory_iterator(root_ / "gc")) {
            const auto name = entry.path().filename().string();
            constexpr std::string_view prefix = "compact-";
            constexpr std::string_view suffix = ".pending";
            if (!name.starts_with(prefix) || !name.ends_with(suffix)) {
                continue;
            }
            const auto values_text = std::string_view(name).substr(
                prefix.size(), name.size() - prefix.size() - suffix.size());
            std::array<std::uint64_t, 3> values{};
            std::size_t value_index = 0;
            std::size_t start = 0;
            bool valid = true;
            while (start <= values_text.size() && value_index < values.size()) {
                const auto end = values_text.find('-', start);
                const auto part = values_text.substr(start, end - start);
                const auto [next, error_code] =
                    std::from_chars(part.data(), part.data() + part.size(), values[value_index]);
                if (error_code != std::errc{} || next != part.data() + part.size()) {
                    valid = false;
                    break;
                }
                ++value_index;
                if (end == std::string_view::npos) {
                    break;
                }
                start = end + 1;
            }
            if (!valid || value_index != values.size() ||
                values[1] > std::numeric_limits<std::uint32_t>::max() ||
                values[2] > std::numeric_limits<std::uint32_t>::max()) {
                std::error_code ignored;
                std::filesystem::remove(entry.path(), ignored);
                continue;
            }
            const auto old_pack_id = static_cast<std::uint32_t>(values[1]);
            const auto new_pack_id = static_cast<std::uint32_t>(values[2]);
            const auto remove_id = manifest.epoch >= values[0] ? old_pack_id : new_pack_id;
            const auto path = root_ / "packs" / ("pack-" + std::to_string(remove_id) + ".pack");
            visit_fault({fault_operation::remove, fault_phase::before, path});
            std::filesystem::remove(path);
            visit_fault({fault_operation::remove, fault_phase::after, path});
            std::filesystem::remove(entry.path());
        }
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), (root_ / "gc").string()));
    }
}

runtime::task<void> store_backend::load_objects() {
    const auto path = root_ / "packs" / "objects.meta";
    if (!std::filesystem::exists(path))
        co_return result<void>{};
    auto raw = co_await read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    const auto bytes = std::span<const std::byte>(*raw);
    const bool version_one =
        bytes.size() >= objects_magic_v1.size() &&
        std::equal(objects_magic_v1.begin(), objects_magic_v1.end(), bytes.begin());
    const bool version_two =
        bytes.size() >= objects_magic_v2.size() &&
        std::equal(objects_magic_v2.begin(), objects_magic_v2.end(), bytes.begin());
    const bool version_three =
        bytes.size() >= objects_magic_v3.size() &&
        std::equal(objects_magic_v3.begin(), objects_magic_v3.end(), bytes.begin());
    const bool version_four =
        bytes.size() >= objects_magic_v4.size() &&
        std::equal(objects_magic_v4.begin(), objects_magic_v4.end(), bytes.begin());
    if ((!version_one && !version_two && !version_three && !version_four) || bytes.size() < 12)
        co_return std::unexpected(store_error(errc::corrupt, "invalid object map", path.string()));
    const auto count = get_u32(bytes, 8);
    std::size_t at = 12;
    for (std::uint32_t number = 0; number < count; ++number) {
        if (at + 41 > bytes.size() || !valid_algorithm(std::to_integer<unsigned char>(bytes[at]))) {
            co_return std::unexpected(
                store_error(errc::corrupt, "truncated object map", path.string()));
        }
        stored_object object{};
        object.id.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
        std::copy_n(bytes.begin() + at, 32, object.id.bytes.begin());
        at += 32;
        object.size = get_u64(bytes, at);
        at += 8;
        if (version_two || version_three || version_four) {
            if (at == bytes.size())
                co_return std::unexpected(
                    store_error(errc::corrupt, "truncated object encryption flag", path.string()));
            const auto encrypted = std::to_integer<unsigned char>(bytes[at++]);
            if (encrypted > 1)
                co_return std::unexpected(
                    store_error(errc::corrupt, "unknown object encryption flag", path.string()));
            if (encrypted == 1) {
                if (at + 2 > bytes.size())
                    co_return std::unexpected(
                        store_error(errc::corrupt, "truncated object key id", path.string()));
                const auto size = get_u16(bytes, at);
                at += 2;
                if (size == 0 || at + size > bytes.size())
                    co_return std::unexpected(
                        store_error(errc::corrupt, "invalid object key id", path.string()));
                object.key_id.assign(reinterpret_cast<const char*>(bytes.data() + at), size);
                at += size;
            }
        }
        if (version_three || version_four) {
            if (at + 35 > bytes.size()) {
                co_return std::unexpected(
                    store_error(errc::corrupt, "truncated object descriptor", path.string()));
            }
            const auto descriptor_algorithm = std::to_integer<unsigned char>(bytes[at++]);
            if (descriptor_algorithm != 0 && !valid_algorithm(descriptor_algorithm)) {
                co_return std::unexpected(
                    store_error(errc::corrupt, "invalid object descriptor", path.string()));
            }
            object.has_descriptor = descriptor_algorithm != 0;
            object.descriptor.alg = static_cast<algorithm>(descriptor_algorithm);
            std::copy_n(bytes.begin() + at, 32, object.descriptor.bytes.begin());
            at += 32;
            const auto kind_size = get_u16(bytes, at);
            at += 2;
            if (kind_size == 0 || at + kind_size > bytes.size()) {
                co_return std::unexpected(
                    store_error(errc::corrupt, "invalid object kind", path.string()));
            }
            object.kind.assign(reinterpret_cast<const char*>(bytes.data() + at), kind_size);
            at += kind_size;
        }
        if (at + 4 > bytes.size())
            co_return std::unexpected(
                store_error(errc::corrupt, "truncated object chunks", path.string()));
        const auto chunks = get_u32(bytes, at);
        at += 4;
        const auto chunk_record_size = version_four ? 41U : 33U;
        if (chunks > (bytes.size() - at) / chunk_record_size)
            co_return std::unexpected(
                store_error(errc::corrupt, "truncated object chunks", path.string()));
        object.chunks.reserve(chunks);
        object.chunk_sizes.reserve(version_four ? chunks : 0);
        for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
            digest id{};
            if (!valid_algorithm(std::to_integer<unsigned char>(bytes[at])))
                co_return std::unexpected(
                    store_error(errc::corrupt, "unknown object chunk algorithm", path.string()));
            id.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
            std::copy_n(bytes.begin() + at, 32, id.bytes.begin());
            at += 32;
            object.chunks.push_back(id);
            if (version_four) {
                object.chunk_sizes.push_back(get_u64(bytes, at));
                at += 8;
            }
        }
        if (version_four) {
            auto total = std::uint64_t{};
            for (const auto size : object.chunk_sizes) {
                if (size > object.size - total) {
                    co_return std::unexpected(
                        store_error(errc::corrupt, "invalid object chunk sizes", path.string()));
                }
                total += size;
            }
            if (total != object.size) {
                co_return std::unexpected(
                    store_error(errc::corrupt, "invalid object chunk sizes", path.string()));
            }
        }
        if (!object.key_id.empty()) {
            for (const auto& chunk : object.chunks)
                chunk_keys_.try_emplace(to_string(chunk), object.key_id);
            if (object.has_descriptor)
                chunk_keys_.try_emplace(to_string(object.descriptor), object.key_id);
        }
        objects_.emplace(to_string(object.id), std::move(object));
    }
    if (at != bytes.size())
        co_return std::unexpected(
            store_error(errc::corrupt, "trailing object map data", path.string()));
    co_return result<void>{};
}

runtime::task<void> store_backend::load_remotes() {
    const auto path = root_ / "packs" / "remotes.meta";
    if (!std::filesystem::exists(path)) {
        co_return result<void>{};
    }
    auto raw = co_await read_file(path);
    if (!raw) {
        co_return std::unexpected(raw.error());
    }
    const auto bytes = std::span<const std::byte>(*raw);
    if (bytes.size() < 12 ||
        !std::equal(remotes_magic_v1.begin(), remotes_magic_v1.end(), bytes.begin())) {
        co_return std::unexpected(
            store_error(errc::corrupt, "invalid remote object map", path.string()));
    }
    const auto count = get_u32(bytes, 8);
    auto at = std::size_t{12};
    for (auto number = std::uint32_t{}; number < count; ++number) {
        if (at + 43 > bytes.size() || !valid_algorithm(std::to_integer<unsigned char>(bytes[at]))) {
            co_return std::unexpected(
                store_error(errc::corrupt, "truncated remote object map", path.string()));
        }
        blob value{};
        value.content.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
        std::copy_n(bytes.begin() + at, value.content.bytes.size(), value.content.bytes.begin());
        at += value.content.bytes.size();
        value.size = get_u64(bytes, at);
        at += 8;
        const auto source_size = get_u16(bytes, at);
        at += 2;
        if (source_size == 0 || at + source_size + 2 > bytes.size()) {
            co_return std::unexpected(
                store_error(errc::corrupt, "invalid remote source", path.string()));
        }
        auto location = remote_blob_location{};
        location.source.assign(reinterpret_cast<const char*>(bytes.data() + at), source_size);
        at += source_size;
        const auto instance_size = get_u16(bytes, at);
        at += 2;
        if (at + instance_size + 4 > bytes.size()) {
            co_return std::unexpected(
                store_error(errc::corrupt, "invalid remote instance", path.string()));
        }
        location.instance.assign(reinterpret_cast<const char*>(bytes.data() + at), instance_size);
        at += instance_size;
        const auto chunks = get_u32(bytes, at);
        at += 4;
        if (chunks == 0 || chunks > (bytes.size() - at) / 41U) {
            co_return std::unexpected(
                store_error(errc::corrupt, "invalid remote chunk list", path.string()));
        }
        value.chunks.reserve(chunks);
        value.chunk_sizes.reserve(chunks);
        auto total = std::uint64_t{};
        for (auto index = std::uint32_t{}; index < chunks; ++index) {
            if (!valid_algorithm(std::to_integer<unsigned char>(bytes[at]))) {
                co_return std::unexpected(
                    store_error(errc::corrupt, "invalid remote chunk digest", path.string()));
            }
            auto chunk = digest{};
            chunk.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
            std::copy_n(bytes.begin() + at, chunk.bytes.size(), chunk.bytes.begin());
            at += chunk.bytes.size();
            const auto chunk_size = get_u64(bytes, at);
            at += 8;
            if (chunk_size > value.size - total) {
                co_return std::unexpected(
                    store_error(errc::corrupt, "invalid remote chunk sizes", path.string()));
            }
            total += chunk_size;
            value.chunks.push_back(chunk);
            value.chunk_sizes.push_back(chunk_size);
        }
        if (total != value.size) {
            co_return std::unexpected(
                store_error(errc::corrupt, "invalid remote chunk sizes", path.string()));
        }
        value.remote = std::move(location);
        remotes_.emplace(to_string(value.content), std::move(value));
    }
    if (at != bytes.size()) {
        co_return std::unexpected(
            store_error(errc::corrupt, "trailing remote object map data", path.string()));
    }
    co_return result<void>{};
}

runtime::task<void> store_backend::save_remotes() {
    auto bytes = std::vector<std::byte>(remotes_magic_v1.begin(), remotes_magic_v1.end());
    put_u32(bytes, static_cast<std::uint32_t>(remotes_.size()));
    for (const auto& [unused, value] : remotes_) {
        if (!value.remote || value.remote->source.empty() ||
            value.remote->source.size() > std::numeric_limits<std::uint16_t>::max() ||
            value.remote->instance.size() > std::numeric_limits<std::uint16_t>::max()) {
            co_return std::unexpected(store_error(
                errc::invalid_argument, "invalid remote blob location", to_string(value.content)));
        }
        bytes.push_back(std::byte{static_cast<unsigned char>(value.content.alg)});
        bytes.insert(bytes.end(), value.content.bytes.begin(), value.content.bytes.end());
        put_u64(bytes, value.size);
        put_u16(bytes, static_cast<std::uint16_t>(value.remote->source.size()));
        bytes.insert(bytes.end(),
                     reinterpret_cast<const std::byte*>(value.remote->source.data()),
                     reinterpret_cast<const std::byte*>(value.remote->source.data() +
                                                        value.remote->source.size()));
        put_u16(bytes, static_cast<std::uint16_t>(value.remote->instance.size()));
        bytes.insert(bytes.end(),
                     reinterpret_cast<const std::byte*>(value.remote->instance.data()),
                     reinterpret_cast<const std::byte*>(value.remote->instance.data() +
                                                        value.remote->instance.size()));
        put_u32(bytes, static_cast<std::uint32_t>(value.chunks.size()));
        for (auto index = std::size_t{}; index < value.chunks.size(); ++index) {
            bytes.push_back(std::byte{static_cast<unsigned char>(value.chunks[index].alg)});
            bytes.insert(
                bytes.end(), value.chunks[index].bytes.begin(), value.chunks[index].bytes.end());
            put_u64(bytes, value.chunk_sizes[index]);
        }
    }
    const auto temporary = root_ / "packs" / "remotes.meta.tmp";
    const auto destination = root_ / "packs" / "remotes.meta";
    auto written = co_await write_file(temporary, std::move(bytes));
    if (!written) {
        co_return std::unexpected(written.error());
    }
    try {
        std::filesystem::rename(temporary, destination);
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), destination.string()));
    }
    co_return result<void>{};
}

runtime::task<void> store_backend::rebuild_remote_record_index() {
    remote_records_.clear();
    auto changed = false;
    for (const auto& [unused, object] : objects_) {
        if (object.kind != "blob") {
            continue;
        }
        auto bytes = co_await get(object.id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_blob(*bytes);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        if (!value->remote) {
            continue;
        }
        remote_records_.emplace(to_string(object.id), value->content);
        changed = remotes_.try_emplace(to_string(value->content), *value).second || changed;
    }
    if (changed) {
        co_return co_await save_remotes();
    }
    co_return result<void>{};
}

runtime::task<void> store_backend::save_objects() {
    auto staged = co_await stage_objects();
    if (!staged)
        co_return std::unexpected(staged.error());
    co_return co_await publish_staged_objects();
}

runtime::task<void> store_backend::rebuild_subject_index() {
    subjects_.clear();
    for (const auto& [unused, object] : objects_) {
        if (object.kind != "attestation") {
            continue;
        }
        auto bytes = co_await get(object.id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_attestation(*bytes);
        if (!value) {
            co_return std::unexpected(store_error(
                errc::corrupt, "indexed attestation cannot be decoded", to_string(object.id)));
        }
        subjects_.add(value->subject, object.id);
    }
    co_return result<void>{};
}

runtime::task<void> store_backend::stage_objects() {
    auto raw_sizes = std::map<std::string, std::uint64_t>{};
    const auto needs_sizes = std::ranges::any_of(objects_, [](const auto& entry) {
        return entry.second.chunk_sizes.empty() && !entry.second.chunks.empty();
    });
    if (needs_sizes) {
        for (const auto& [unused, pack] : packs_) {
            for (const auto& entry : pack.entries)
                raw_sizes.try_emplace(to_string(entry.id), entry.raw_length);
        }
    }
    std::vector<std::byte> bytes(objects_magic_v4.begin(), objects_magic_v4.end());
    put_u32(bytes, static_cast<std::uint32_t>(objects_.size()));
    for (auto& [unused, object] : objects_) {
        if (object.chunk_sizes.empty() && !object.chunks.empty()) {
            for (const auto& chunk : object.chunks) {
                const auto size = raw_sizes.find(to_string(chunk));
                if (size == raw_sizes.end()) {
                    co_return std::unexpected(store_error(
                        errc::corrupt, "object references a missing chunk", to_string(object.id)));
                }
                object.chunk_sizes.push_back(size->second);
            }
        }
        if (object.chunk_sizes.size() != object.chunks.size()) {
            co_return std::unexpected(store_error(
                errc::corrupt, "object chunk sizes are incomplete", to_string(object.id)));
        }
        bytes.push_back(std::byte{static_cast<unsigned char>(object.id.alg)});
        bytes.insert(bytes.end(), object.id.bytes.begin(), object.id.bytes.end());
        put_u64(bytes, object.size);
        bytes.push_back(std::byte{static_cast<unsigned char>(!object.key_id.empty())});
        if (!object.key_id.empty()) {
            put_u16(bytes, static_cast<std::uint16_t>(object.key_id.size()));
            bytes.insert(
                bytes.end(),
                reinterpret_cast<const std::byte*>(object.key_id.data()),
                reinterpret_cast<const std::byte*>(object.key_id.data() + object.key_id.size()));
        }
        bytes.push_back(object.has_descriptor
                            ? std::byte{static_cast<unsigned char>(object.descriptor.alg)}
                            : std::byte{0});
        bytes.insert(bytes.end(), object.descriptor.bytes.begin(), object.descriptor.bytes.end());
        put_u16(bytes, static_cast<std::uint16_t>(object.kind.size()));
        bytes.insert(bytes.end(),
                     reinterpret_cast<const std::byte*>(object.kind.data()),
                     reinterpret_cast<const std::byte*>(object.kind.data() + object.kind.size()));
        put_u32(bytes, static_cast<std::uint32_t>(object.chunks.size()));
        for (std::size_t index = 0; index < object.chunks.size(); ++index) {
            const auto& chunk = object.chunks[index];
            bytes.push_back(std::byte{static_cast<unsigned char>(chunk.alg)});
            bytes.insert(bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
            put_u64(bytes, object.chunk_sizes[index]);
        }
    }
    co_return co_await write_file(root_ / "packs" / "objects.meta.tmp", std::move(bytes));
}

runtime::task<void> store_backend::publish_staged_objects() {
    const auto temporary = root_ / "packs" / "objects.meta.tmp";
    const auto destination = root_ / "packs" / "objects.meta";
    try {
        visit_fault({fault_operation::rename, fault_phase::before, temporary});
        std::filesystem::rename(temporary, destination);
        visit_fault({fault_operation::rename, fault_phase::after, destination});
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), destination.string()));
    }
}

runtime::task<void> store_backend::index_packs(bool rebuild) {
    auto collection = co_await load_collection_manifest(root_);
    if (!collection)
        co_return std::unexpected(collection.error());
    std::vector<index_entry> all;
    std::uint32_t highest = index_.highest_pack_id();
    try {
        for (const auto& file : std::filesystem::directory_iterator(root_ / "packs")) {
            const auto name = file.path().filename().string();
            if (!file.is_regular_file() || !name.starts_with("pack-") || !name.ends_with(".pack"))
                continue;
            const auto digits = name.substr(5, name.size() - 10);
            const auto id = static_cast<std::uint32_t>(std::stoul(digits));
            auto pack = co_await read_pack(file.path(), id);
            if (!pack)
                co_return std::unexpected(pack.error());
            highest = std::max(highest, id);
            for (std::size_t frame = 0; frame != pack->entries.size(); ++frame) {
                const auto& entry = pack->entries[frame];
                if (!collection->dead(id, frame))
                    all.push_back({entry.id, entry.location});
            }
            packs_.emplace(id, std::move(*pack));
        }
    } catch (const std::exception& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), (root_ / "packs").string()));
    }
    current_pack_ = highest == 0 ? 1 : highest;
    if (!packs_.contains(current_pack_)) {
        packs_.emplace(current_pack_, pack_contents{current_pack_, {}, 16});
    } else if (!packs_.at(current_pack_).appendable) {
        ++current_pack_;
        packs_.emplace(current_pack_, pack_contents{current_pack_, {}, 16});
    }
    if (rebuild) {
        index_.set_highest_pack_id(highest);
        index_.set_collection_epoch(collection->epoch);
        co_return co_await index_.rebuild(std::move(all));
    }
    for (auto& entry : all) {
        auto inserted = index_.insert(std::move(entry));
        if (!inserted)
            co_return std::unexpected(inserted.error());
        if (index_.needs_flush(entry.id.alg)) {
            auto flushed = co_await index_.flush();
            if (!flushed)
                co_return std::unexpected(flushed.error());
        }
    }
    index_.set_highest_pack_id(highest);
    co_return co_await index_.flush();
}

runtime::task<pack_entry> store_backend::store_chunk(
    digest id, std::vector<std::byte> raw, std::optional<crypto_detail::domain_keys> encryption) {
    auto present = index_.find(id);
    if (!present)
        co_return std::unexpected(present.error());
    if (*present)
        co_return pack_entry{id, **present, static_cast<std::uint32_t>(raw.size())};
    const auto limit = options_.pack_size == 0 ? 64ULL * 1024ULL * 1024ULL : options_.pack_size;
    auto& destination = packs_.at(current_pack_);
    auto written = co_await append_pack_entry(
        root_ / "packs" / ("pack-" + std::to_string(current_pack_) + ".pack"),
        destination,
        id,
        std::move(raw),
        encryption ? std::optional{encryption->chunk_encryption} : std::nullopt,
        options_.durable);
    if (!written)
        co_return std::unexpected(written.error());
    destination.entries.push_back(*written);
    destination.data_end += 49 + written->location.length;
    auto inserted = index_.insert({id, written->location});
    if (!inserted)
        co_return std::unexpected(inserted.error());
    if (index_.needs_flush(id.alg)) {
        auto flushed = co_await index_.flush();
        if (!flushed)
            co_return std::unexpected(flushed.error());
    }
    if (destination.data_end >= limit) {
        auto sealed = co_await seal_pack(root_ / "packs" /
                                             ("pack-" + std::to_string(current_pack_) + ".pack"),
                                         destination,
                                         options_.durable);
        if (!sealed)
            co_return std::unexpected(sealed.error());
        destination.appendable = false;
        ++current_pack_;
        packs_.emplace(current_pack_, pack_contents{current_pack_, {}, 16});
    }
    co_return *written;
}

runtime::task<std::vector<std::byte>>
store_backend::load_chunk(digest id, std::optional<crypto_detail::domain_keys> encryption) {
    auto found = index_.find(id);
    if (!found)
        co_return std::unexpected(found.error());
    if (!*found)
        co_return std::unexpected(
            store_error(errc::not_found, "chunk is not present", to_string(id)));
    auto raw = co_await read_pack_entry(
        root_ / "packs" / ("pack-" + std::to_string((*found)->pack_id) + ".pack"),
        **found,
        id,
        encryption ? std::optional{encryption->chunk_encryption} : std::nullopt);
    if (!raw)
        co_return std::unexpected(raw.error());
    const auto actual =
        encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, *raw) : hash(id.alg, *raw);
    if (!actual || actual->bytes != id.bytes || actual->alg != id.alg) {
        co_return std::unexpected(
            store_error(errc::corrupt, "chunk digest verification failed", to_string(id)));
    }
    co_return raw;
}

runtime::task<std::vector<std::byte>>
store_backend::load_snapshot_object(const stored_object& object,
                                    const std::map<std::string, chunk_location>& locations) {
    std::optional<crypto_detail::domain_keys> encryption;
    if (!object.key_id.empty()) {
        if (!options_.keys) {
            co_return std::unexpected(store_error(
                errc::unauthorized, "encrypted object key is unavailable", object.key_id));
        }
        auto data = co_await options_.keys->data_key(object.key_id, false);
        if (!data)
            co_return std::unexpected(data.error());
        auto derived = crypto_detail::derive_domain_keys(*data);
        crypto_detail::wipe(*data);
        if (!derived)
            co_return std::unexpected(derived.error());
        encryption = std::move(*derived);
    }
    std::vector<std::byte> bytes;
    bytes.reserve(object.size);
    for (const auto& chunk : object.chunks) {
        const auto location = locations.find(to_string(chunk));
        if (location == locations.end()) {
            co_return std::unexpected(store_error(
                errc::corrupt, "object references a missing chunk", to_string(object.id)));
        }
        auto raw = co_await read_pack_entry(
            root_ / "packs" / ("pack-" + std::to_string(location->second.pack_id) + ".pack"),
            location->second,
            chunk,
            encryption ? std::optional{encryption->chunk_encryption} : std::nullopt);
        if (!raw)
            co_return std::unexpected(raw.error());
        bytes.insert(bytes.end(), raw->begin(), raw->end());
    }
    if (bytes.size() != object.size) {
        co_return std::unexpected(store_error(
            errc::corrupt, "object length differs from chunk list", to_string(object.id)));
    }
    co_return bytes;
}

runtime::task<void> store_backend::add_object(stored_object object) {
    if (!object.key_id.empty()) {
        for (const auto& chunk : object.chunks)
            chunk_keys_.try_emplace(to_string(chunk), object.key_id);
        if (object.has_descriptor)
            chunk_keys_.try_emplace(to_string(object.descriptor), object.key_id);
    }
    objects_[to_string(object.id)] = std::move(object);
    objects_dirty_ = true;
    if (object_batch_depth_ != 0) {
        co_return result<void>{};
    }
    objects_dirty_ = false;
    co_return co_await save_objects();
}

void store_backend::begin_object_batch() {
    ++object_batch_depth_;
}

void store_backend::abort_object_batch() {
    object_batch_depth_ = 0;
    objects_dirty_ = false;
}

runtime::task<void> store_backend::commit_object_batch() {
    if (object_batch_depth_ == 0) {
        co_return std::unexpected(
            store_error(errc::invalid_argument, "object batch is not active", "store::batch"));
    }
    --object_batch_depth_;
    if (object_batch_depth_ != 0 || !objects_dirty_) {
        co_return result<void>{};
    }
    objects_dirty_ = false;
    co_return co_await save_objects();
}

runtime::task<digest> store_backend::put(std::vector<std::byte> input) {
    auto key_id = std::optional<std::string>{};
    if (options_.keys) {
        if (options_.write_key.empty()) {
            auto default_key = options_.keys->default_key_id();
            if (!default_key) {
                co_return std::unexpected(default_key.error());
            }
            key_id = std::move(*default_key);
        } else {
            key_id = options_.write_key;
        }
        auto versioned = co_await options_.keys->write_key(*key_id);
        if (!versioned)
            co_return std::unexpected(versioned.error());
        key_id = std::move(*versioned);
    }
    co_return co_await put_selected(std::move(input), std::move(key_id), options_.hash_algorithm);
}

runtime::task<digest> store_backend::put_plain(std::vector<std::byte> input) {
    co_return co_await put_selected(std::move(input), std::nullopt, options_.hash_algorithm);
}

runtime::task<digest> store_backend::put_plain(std::vector<std::byte> input,
                                               algorithm hash_algorithm) {
    if (hash_algorithm != algorithm::blake3 && hash_algorithm != algorithm::sha256) {
        co_return std::unexpected(
            store_error(errc::invalid_argument, "unsupported object hash algorithm", "store::put"));
    }
    co_return co_await put_selected(std::move(input), std::nullopt, hash_algorithm);
}

runtime::task<digest> store_backend::put_encrypted(std::vector<std::byte> input,
                                                   std::string key_id) {
    if (key_id.empty()) {
        co_return std::unexpected(
            store_error(errc::invalid_argument, "encrypted put requires a key id", "store::put"));
    }
    if (!options_.keys) {
        co_return std::unexpected(
            store_error(errc::unauthorized, "encrypted object keyring is unavailable", key_id));
    }
    auto versioned = co_await options_.keys->write_key(key_id);
    if (!versioned) {
        co_return std::unexpected(versioned.error());
    }
    co_return co_await put_selected(
        std::move(input), std::move(*versioned), options_.hash_algorithm);
}

runtime::task<digest> store_backend::put_selected(std::vector<std::byte> input,
                                                  std::optional<std::string> selected_key,
                                                  algorithm hash_algorithm) {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error)
        co_return std::unexpected(store_error(errc::busy, lock_error.message(), "store::put"));
    std::optional<crypto_detail::domain_keys> encryption;
    auto remote_record = std::optional<blob>{};
    if (!selected_key) {
        auto decoded = objects::decode_blob(input);
        if (decoded && decoded->remote) {
            remote_record = std::move(*decoded);
        }
    }
    auto key_id = std::string{};
    if (selected_key) {
        if (!options_.keys) {
            co_return std::unexpected(store_error(
                errc::unauthorized, "encrypted object keyring is unavailable", *selected_key));
        }
        key_id = std::move(*selected_key);
        auto data = co_await options_.keys->data_key(key_id, true);
        if (!data)
            co_return std::unexpected(data.error());
        auto derived = crypto_detail::derive_domain_keys(*data);
        crypto_detail::wipe(*data);
        if (!derived)
            co_return std::unexpected(derived.error());
        encryption = std::move(*derived);
    }
    digest whole{};
    const auto algorithm_value = hash_algorithm;
    std::vector<chunk_range> ranges;
    if (!encryption && input.size() > 4U * 1024U * 1024U) {
        auto hashed = co_await runtime::offload([input, algorithm_value] {
            const auto output = hash(algorithm_value, input);
            if (!output)
                throw std::runtime_error(output.error().message);
            return *output;
        });
        if (!hashed)
            co_return std::unexpected(hashed.error());
        whole = *hashed;
        auto chunked = co_await runtime::offload([input] { return fastcdc(input); });
        if (!chunked)
            co_return std::unexpected(chunked.error());
        ranges = std::move(*chunked);
    } else {
        const auto hashed = encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, input)
                                       : hash(algorithm_value, input);
        if (!hashed)
            co_return std::unexpected(hashed.error());
        whole = *hashed;
        ranges = fastcdc(input);
    }
    if (objects_.contains(to_string(whole))) {
        if (remote_record) {
            remote_records_[to_string(whole)] = remote_record->content;
            remotes_[to_string(remote_record->content)] = *remote_record;
            auto saved_remotes = co_await save_remotes();
            if (!saved_remotes) {
                co_return std::unexpected(saved_remotes.error());
            }
        }
        co_return whole;
    }
    stored_object object;
    object.id = whole;
    object.size = input.size();
    object.key_id = key_id;
    object.kind = detect_object_kind(input);
    auto attestation_subject = std::optional<digest>{};
    if (object.kind == "attestation") {
        auto value = objects::decode_attestation(input);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        attestation_subject = value->subject;
    }
    for (const auto part : ranges) {
        const auto bytes = std::span<const std::byte>(input).subspan(part.offset, part.size);
        const auto id = encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, bytes)
                                   : hash(algorithm_value, bytes);
        if (!id)
            co_return std::unexpected(id.error());
        object.chunks.push_back(*id);
        object.chunk_sizes.push_back(part.size);
    }
    if (options_.write_through) {
        auto written =
            co_await options_.write_through->put(whole, input, object.chunks, object.chunk_sizes);
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    for (auto index = std::size_t{}; index < ranges.size(); ++index) {
        const auto part = ranges[index];
        const auto bytes = std::span<const std::byte>(input).subspan(part.offset, part.size);
        auto stored = co_await store_chunk(
            object.chunks[index], std::vector<std::byte>(bytes.begin(), bytes.end()), encryption);
        if (!stored)
            co_return std::unexpected(stored.error());
    }
    auto descriptor = encode_chunk_list(object.size, key_id, object.chunks);
    const auto descriptor_id = encryption
                                   ? crypto_detail::keyed_digest(encryption->chunk_mac, descriptor)
                                   : hash(algorithm_value, descriptor);
    if (!descriptor_id)
        co_return std::unexpected(descriptor_id.error());
    auto descriptor_stored =
        co_await store_chunk(*descriptor_id, std::move(descriptor), encryption);
    if (!descriptor_stored)
        co_return std::unexpected(descriptor_stored.error());
    object.descriptor = *descriptor_id;
    object.has_descriptor = true;
    auto saved = co_await add_object(std::move(object));
    if (!saved)
        co_return std::unexpected(saved.error());
    if (remote_record) {
        remote_records_[to_string(whole)] = remote_record->content;
        remotes_[to_string(remote_record->content)] = *remote_record;
        auto saved_remotes = co_await save_remotes();
        if (!saved_remotes) {
            co_return std::unexpected(saved_remotes.error());
        }
    }
    if (attestation_subject) {
        subjects_.add(*attestation_subject, whole);
    }
    index_.set_highest_pack_id(descriptor_stored->location.pack_id);
    auto flushed = co_await index_.flush();
    if (!flushed)
        co_return std::unexpected(flushed.error());
    co_return whole;
}

runtime::task<stored_object>
store_backend::scan_file(std::filesystem::path source,
                         std::optional<crypto_detail::domain_keys> encryption,
                         std::string key_id,
                         bool store_chunks) {
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::corosio::random_access_file file(executor.context());
    file.open(source, boost::corosio::file_base::read_only);
    const auto source_size = file.size();
    auto hash_key = std::optional<crypto_detail::key>{};
    if (encryption) {
        hash_key = encryption->chunk_mac;
    }
    auto whole_hasher = incremental_digest{options_.hash_algorithm, hash_key};
    auto streaming = chunker{};
    auto object = stored_object{};
    object.size = source_size;
    object.key_id = std::move(key_id);
    auto window = std::vector<std::byte>(
        static_cast<std::size_t>(std::min<std::uint64_t>(file_window_size, source_size)));
    auto offset = std::uint64_t{};
    while (offset < source_size) {
        const auto wanted =
            static_cast<std::size_t>(std::min<std::uint64_t>(window.size(), source_size - offset));
        auto read = std::size_t{};
        while (read < wanted) {
            auto [ec, count] = co_await file.read_some_at(
                offset + read, boost::capy::mutable_buffer(window.data() + read, wanted - read));
            if (ec || count == 0) {
                co_return std::unexpected(store_error(
                    errc::io, ec ? ec.message() : "unexpected end of file", source.string()));
            }
            read += count;
        }
        const auto input = std::span<const std::byte>(window).first(wanted);
        whole_hasher.update(input);
        for (auto& chunk : streaming.feed(input)) {
            const auto id = encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, chunk)
                                       : hash(options_.hash_algorithm, chunk);
            if (!id) {
                co_return std::unexpected(id.error());
            }
            const auto chunk_size = chunk.size();
            if (store_chunks) {
                auto stored = co_await store_chunk(*id, std::move(chunk), encryption);
                if (!stored) {
                    co_return std::unexpected(stored.error());
                }
            }
            object.chunks.push_back(*id);
            object.chunk_sizes.push_back(chunk_size);
        }
        offset += wanted;
    }
    if (source_size <= window.size()) {
        object.kind = detect_object_kind(std::span<const std::byte>(window).first(window.size()));
    }
    file.close();
    for (auto& chunk : streaming.finish()) {
        const auto id = encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, chunk)
                                   : hash(options_.hash_algorithm, chunk);
        if (!id) {
            co_return std::unexpected(id.error());
        }
        const auto chunk_size = chunk.size();
        if (store_chunks) {
            auto stored = co_await store_chunk(*id, std::move(chunk), encryption);
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
        }
        object.chunks.push_back(*id);
        object.chunk_sizes.push_back(chunk_size);
    }
    object.id = whole_hasher.finish();
    co_return object;
}

runtime::task<digest> store_backend::put_file(std::filesystem::path source) {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error) {
        co_return std::unexpected(store_error(errc::busy, lock_error.message(), "store::put_file"));
    }
    try {
        auto encryption = std::optional<crypto_detail::domain_keys>{};
        auto key_id = std::string{};
        if (options_.keys) {
            if (options_.write_key.empty()) {
                auto default_key = options_.keys->default_key_id();
                if (!default_key) {
                    co_return std::unexpected(default_key.error());
                }
                key_id = std::move(*default_key);
            } else {
                key_id = options_.write_key;
            }
            auto versioned = co_await options_.keys->write_key(key_id);
            if (!versioned) {
                co_return std::unexpected(versioned.error());
            }
            key_id = std::move(*versioned);
            auto data = co_await options_.keys->data_key(key_id, true);
            if (!data) {
                co_return std::unexpected(data.error());
            }
            auto derived = crypto_detail::derive_domain_keys(*data);
            crypto_detail::wipe(*data);
            if (!derived) {
                co_return std::unexpected(derived.error());
            }
            encryption = std::move(*derived);
        }

        const auto writes_through = static_cast<bool>(options_.write_through);
        auto scanned = co_await scan_file(source, encryption, key_id, !writes_through);
        if (!scanned) {
            co_return std::unexpected(scanned.error());
        }
        auto object = std::move(*scanned);
        if (objects_.contains(to_string(object.id))) {
            co_return object.id;
        }

        if (writes_through) {
            auto mapping = mapped_file{};
            auto input = std::span<const std::byte>{};
            if (object.size != 0) {
                auto opened = mapping.open(source);
                if (!opened) {
                    co_return std::unexpected(opened.error());
                }
                input = mapping.bytes();
            }
            if (input.size() != object.size) {
                co_return std::unexpected(store_error(
                    errc::io, "source size changed while it was read", source.string()));
            }

            const auto expected_chunks = object.chunks;
            const auto expected_sizes = object.chunk_sizes;
            const auto hash_algorithm = options_.hash_algorithm;
            auto verified = co_await runtime::offload(
                [input, expected_chunks, expected_sizes, encryption, hash_algorithm] {
                    auto offset = std::size_t{};
                    for (auto index = std::size_t{}; index < expected_chunks.size(); ++index) {
                        const auto size = static_cast<std::size_t>(expected_sizes[index]);
                        const auto chunk = input.subspan(offset, size);
                        const auto actual =
                            encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, chunk)
                                       : hash(hash_algorithm, chunk);
                        if (!actual || actual->alg != expected_chunks[index].alg ||
                            actual->bytes != expected_chunks[index].bytes) {
                            return false;
                        }
                        offset += size;
                    }
                    return offset == input.size();
                });
            if (!verified) {
                co_return std::unexpected(verified.error());
            }
            if (!*verified) {
                co_return std::unexpected(
                    store_error(errc::io, "source changed while it was read", source.string()));
            }

            auto written = co_await options_.write_through->put(
                object.id, input, object.chunks, object.chunk_sizes);
            if (!written) {
                co_return std::unexpected(written.error());
            }

            auto offset = std::size_t{};
            for (auto index = std::size_t{}; index < object.chunks.size(); ++index) {
                const auto size = static_cast<std::size_t>(object.chunk_sizes[index]);
                const auto chunk = input.subspan(offset, size);
                auto stored =
                    co_await store_chunk(object.chunks[index],
                                         std::vector<std::byte>(chunk.begin(), chunk.end()),
                                         encryption);
                if (!stored) {
                    co_return std::unexpected(stored.error());
                }
                offset += size;
            }
        }

        auto descriptor = encode_chunk_list(object.size, key_id, object.chunks);
        const auto descriptor_id =
            encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, descriptor)
                       : hash(options_.hash_algorithm, descriptor);
        if (!descriptor_id) {
            co_return std::unexpected(descriptor_id.error());
        }
        auto descriptor_stored =
            co_await store_chunk(*descriptor_id, std::move(descriptor), encryption);
        if (!descriptor_stored) {
            co_return std::unexpected(descriptor_stored.error());
        }
        object.descriptor = *descriptor_id;
        object.has_descriptor = true;
        const auto whole = object.id;
        auto saved = co_await add_object(std::move(object));
        if (!saved) {
            co_return std::unexpected(saved.error());
        }
        index_.set_highest_pack_id(descriptor_stored->location.pack_id);
        auto flushed = co_await index_.flush();
        if (!flushed) {
            co_return std::unexpected(flushed.error());
        }
        co_return whole;
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), source.string()));
    } catch (const std::exception& caught) {
        co_return std::unexpected(store_error(errc::internal, caught.what(), source.string()));
    }
}

runtime::task<void> store_backend::get_to_file(digest id, std::filesystem::path destination) {
    const auto it = objects_.find(to_string(id));
    if (it == objects_.end())
        co_return std::unexpected(
            store_error(errc::not_found, "object is not present", to_string(id)));
    try {
        std::optional<crypto_detail::domain_keys> encryption;
        if (!it->second.key_id.empty()) {
            if (!options_.keys) {
                co_return std::unexpected(store_error(
                    errc::unauthorized, "encrypted object key is unavailable", it->second.key_id));
            }
            auto data = co_await options_.keys->data_key(it->second.key_id, false);
            if (!data)
                co_return std::unexpected(data.error());
            auto derived = crypto_detail::derive_domain_keys(*data);
            crypto_detail::wipe(*data);
            if (!derived)
                co_return std::unexpected(derived.error());
            encryption = std::move(*derived);
        }
        auto hash_key = std::optional<crypto_detail::key>{};
        if (encryption)
            hash_key = encryption->chunk_mac;
        auto whole_hasher = incremental_digest{id.alg, hash_key};
        const auto executor = co_await boost::capy::this_coro::executor;
        boost::corosio::random_access_file file(executor.context());
        file.open(destination,
                  boost::corosio::file_base::read_write | boost::corosio::file_base::create |
                      boost::corosio::file_base::truncate);
        auto offset = std::uint64_t{};
        auto cached_id = std::optional<digest>{};
        auto cached_bytes = std::vector<std::byte>{};
        for (const auto& chunk : it->second.chunks) {
            if (!cached_id || cached_id->alg != chunk.alg || cached_id->bytes != chunk.bytes) {
                auto raw = co_await load_chunk(chunk, encryption);
                if (!raw)
                    co_return std::unexpected(raw.error());
                cached_id = chunk;
                cached_bytes = std::move(*raw);
            }
            whole_hasher.update(cached_bytes);
            auto written = std::size_t{};
            while (written < cached_bytes.size()) {
                auto [ec, count] = co_await file.write_some_at(
                    offset + written,
                    boost::capy::const_buffer(cached_bytes.data() + written,
                                              cached_bytes.size() - written));
                if (ec || count == 0) {
                    co_return std::unexpected(store_error(
                        errc::io, ec ? ec.message() : "short file write", destination.string()));
                }
                written += count;
            }
            offset += cached_bytes.size();
        }
        file.close();
        if (offset != it->second.size) {
            co_return std::unexpected(store_error(
                errc::corrupt, "object length differs from its chunk list", to_string(id)));
        }
        const auto actual = whole_hasher.finish();
        if (actual.alg != id.alg || actual.bytes != id.bytes) {
            co_return std::unexpected(
                store_error(errc::corrupt, "object digest verification failed", to_string(id)));
        }
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), destination.string()));
    } catch (const std::exception& caught) {
        co_return std::unexpected(store_error(errc::internal, caught.what(), destination.string()));
    }
}

result<blob> store_backend::blob_for(digest id) const {
    const auto found = objects_.find(to_string(id));
    if (found == objects_.end()) {
        return std::unexpected(
            store_error(errc::not_found, "object is not present", to_string(id)));
    }
    if (found->second.chunks.size() != found->second.chunk_sizes.size()) {
        return std::unexpected(
            store_error(errc::corrupt, "object chunk sizes are unavailable", to_string(id)));
    }
    return blob{found->second.chunks, found->second.size, found->second.chunk_sizes, id};
}

runtime::task<std::vector<std::byte>>
store_backend::hydrate_remote_chunk(const blob& value, std::size_t index, std::uint64_t offset) {
    if (!value.remote || index >= value.chunks.size() || index >= value.chunk_sizes.size()) {
        co_return std::unexpected(
            store_error(errc::corrupt, "invalid remote chunk metadata", to_string(value.content)));
    }
    auto local = index_.find(value.chunks[index]);
    if (!local) {
        co_return std::unexpected(local.error());
    }
    if (*local) {
        co_return co_await load_chunk(value.chunks[index]);
    }
    const auto source = remote_sources_.find(value.remote->source);
    if (source == remote_sources_.end() || !source->second) {
        co_return std::unexpected(store_error(
            errc::unavailable, "remote blob source is unavailable", value.remote->source));
    }
    auto bytes = std::vector<std::byte>(static_cast<std::size_t>(value.chunk_sizes[index]));
    auto received = std::uint64_t{};
    while (received < value.chunk_sizes[index]) {
        auto read = co_await source->second->read_range(
            value.remote->instance,
            value.content,
            offset + received,
            std::span<std::byte>(bytes).subspan(static_cast<std::size_t>(received)));
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0 || *read > value.chunk_sizes[index] - received) {
            co_return std::unexpected(store_error(
                errc::corrupt, "remote source returned a short range", to_string(value.content)));
        }
        received += *read;
    }
    auto actual = hash(value.chunks[index].alg, bytes);
    if (!actual || actual->alg != value.chunks[index].alg ||
        actual->bytes != value.chunks[index].bytes) {
        co_return std::unexpected(store_error(errc::corrupt,
                                              "remote chunk failed digest verification",
                                              to_string(value.chunks[index])));
    }
    {
        auto [lock_error, guard] = co_await mutation_.scoped_lock();
        if (lock_error) {
            co_return std::unexpected(
                store_error(errc::busy, lock_error.message(), "remote chunk hydration"));
        }
        auto stored = co_await store_chunk(value.chunks[index], bytes);
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
    }
    if (options_.on_hydration) {
        auto observed = co_await options_.on_hydration(value.content,
                                                       value.chunks[index],
                                                       value.chunk_sizes[index],
                                                       index + 1 == value.chunks.size());
        if (!observed) {
            co_return std::unexpected(observed.error());
        }
    }
    co_return bytes;
}

runtime::task<std::vector<std::byte>> store_backend::get_remote(const blob& value) {
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(value.size));
    auto offset = std::uint64_t{};
    for (auto index = std::size_t{}; index < value.chunks.size(); ++index) {
        auto chunk = co_await hydrate_remote_chunk(value, index, offset);
        if (!chunk) {
            co_return std::unexpected(chunk.error());
        }
        output.insert(output.end(), chunk->begin(), chunk->end());
        offset += value.chunk_sizes[index];
    }
    if (output.size() != value.size) {
        co_return std::unexpected(store_error(
            errc::corrupt, "remote object length differs from metadata", to_string(value.content)));
    }
    auto actual = hash(value.content.alg, output);
    if (!actual || actual->alg != value.content.alg || actual->bytes != value.content.bytes) {
        co_return std::unexpected(store_error(
            errc::corrupt, "remote object failed digest verification", to_string(value.content)));
    }
    co_return output;
}

runtime::task<std::uint64_t>
store_backend::read_remote(const blob& value, std::uint64_t offset, std::span<std::byte> into) {
    if (offset >= value.size || into.empty()) {
        co_return std::uint64_t{};
    }
    const auto wanted = std::min<std::uint64_t>(into.size(), value.size - offset);
    auto position = std::uint64_t{};
    auto copied = std::uint64_t{};
    for (auto index = std::size_t{}; index < value.chunks.size(); ++index) {
        const auto end = position + value.chunk_sizes[index];
        if (end > offset && copied < wanted) {
            auto chunk = co_await hydrate_remote_chunk(value, index, position);
            if (!chunk) {
                co_return std::unexpected(chunk.error());
            }
            const auto begin = static_cast<std::size_t>(std::max(position, offset) - position);
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(chunk->size() - begin, wanted - copied));
            std::copy_n(chunk->begin() + begin, amount, into.begin() + copied);
            copied += amount;
        }
        position = end;
        if (copied == wanted) {
            break;
        }
    }
    if (copied != wanted) {
        co_return std::unexpected(store_error(
            errc::corrupt, "remote chunk list does not cover range", to_string(value.content)));
    }
    co_return copied;
}

runtime::task<std::vector<std::byte>> store_backend::get(digest id) {
    const auto it = objects_.find(to_string(id));
    if (it == objects_.end()) {
        const auto remote = remotes_.find(to_string(id));
        if (remote != remotes_.end()) {
            co_return co_await get_remote(remote->second);
        }
        std::optional<crypto_detail::domain_keys> encryption;
        if (id.alg == algorithm::blake3_keyed) {
            const auto key = chunk_keys_.find(to_string(id));
            if (key == chunk_keys_.end() || !options_.keys) {
                co_return std::unexpected(store_error(
                    errc::unauthorized, "encrypted chunk key is unavailable", to_string(id)));
            }
            auto data = co_await options_.keys->data_key(key->second, false);
            if (!data)
                co_return std::unexpected(data.error());
            auto derived = crypto_detail::derive_domain_keys(*data);
            crypto_detail::wipe(*data);
            if (!derived)
                co_return std::unexpected(derived.error());
            encryption = std::move(*derived);
        }
        co_return co_await load_chunk(id, encryption);
    }
    std::optional<crypto_detail::domain_keys> encryption;
    if (!it->second.key_id.empty()) {
        if (!options_.keys)
            co_return std::unexpected(store_error(
                errc::unauthorized, "encrypted object key is unavailable", it->second.key_id));
        auto data = co_await options_.keys->data_key(it->second.key_id, false);
        if (!data)
            co_return std::unexpected(data.error());
        auto derived = crypto_detail::derive_domain_keys(*data);
        crypto_detail::wipe(*data);
        if (!derived)
            co_return std::unexpected(derived.error());
        encryption = std::move(*derived);
    }
    std::vector<std::byte> out;
    out.reserve(it->second.size);
    for (const auto& chunk : it->second.chunks) {
        auto raw = co_await load_chunk(chunk, encryption);
        if (!raw)
            co_return std::unexpected(raw.error());
        out.insert(out.end(), raw->begin(), raw->end());
    }
    if (out.size() != it->second.size)
        co_return std::unexpected(
            store_error(errc::corrupt, "object length differs from its chunk list", to_string(id)));
    const auto actual =
        encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, out) : hash(id.alg, out);
    if (!actual || actual->bytes != id.bytes || actual->alg != id.alg)
        co_return std::unexpected(
            store_error(errc::corrupt, "object digest verification failed", to_string(id)));
    co_return out;
}

runtime::task<std::uint64_t>
store_backend::read(digest id, std::uint64_t offset, std::span<std::byte> into) {
    const auto it = objects_.find(to_string(id));
    if (it == objects_.end()) {
        const auto remote = remotes_.find(to_string(id));
        if (remote != remotes_.end()) {
            co_return co_await read_remote(remote->second, offset, into);
        }
        auto raw = co_await get(id);
        if (!raw)
            co_return std::unexpected(raw.error());
        if (offset >= raw->size() || into.empty())
            co_return std::uint64_t{0};
        const auto amount =
            static_cast<std::size_t>(std::min<std::uint64_t>(into.size(), raw->size() - offset));
        std::copy_n(raw->begin() + static_cast<std::size_t>(offset), amount, into.begin());
        co_return amount;
    }
    std::optional<crypto_detail::domain_keys> encryption;
    if (!it->second.key_id.empty()) {
        if (!options_.keys)
            co_return std::unexpected(store_error(
                errc::unauthorized, "encrypted object key is unavailable", it->second.key_id));
        auto data = co_await options_.keys->data_key(it->second.key_id, false);
        if (!data)
            co_return std::unexpected(data.error());
        auto derived = crypto_detail::derive_domain_keys(*data);
        crypto_detail::wipe(*data);
        if (!derived)
            co_return std::unexpected(derived.error());
        encryption = std::move(*derived);
    }
    if (offset >= it->second.size || into.empty())
        co_return std::uint64_t{0};
    const auto wanted = std::min<std::uint64_t>(into.size(), it->second.size - offset);
    std::uint64_t position = 0;
    std::uint64_t copied = 0;
    const auto has_sizes = it->second.chunk_sizes.size() == it->second.chunks.size();
    for (std::size_t index = 0; index < it->second.chunks.size(); ++index) {
        const auto& chunk = it->second.chunks[index];
        if (has_sizes) {
            const auto end = position + it->second.chunk_sizes[index];
            if (end <= offset) {
                position = end;
                continue;
            }
        }
        auto raw = co_await load_chunk(chunk, encryption);
        if (!raw)
            co_return std::unexpected(raw.error());
        const auto end = position + raw->size();
        if (end > offset && copied < wanted) {
            const auto begin = static_cast<std::size_t>(std::max(position, offset) - position);
            const auto amount = static_cast<std::size_t>(
                std::min<std::uint64_t>(end - (position + begin), wanted - copied));
            std::copy_n(raw->begin() + begin, amount, into.begin() + copied);
            copied += amount;
        }
        position = end;
        if (copied == wanted)
            break;
    }
    co_return copied;
}

runtime::task<bool> store_backend::contains(digest id) {
    const auto object = objects_.find(to_string(id));
    if (object != objects_.end()) {
        if (!object->second.key_id.empty()) {
            if (!options_.keys)
                co_return std::unexpected(store_error(errc::unauthorized,
                                                      "encrypted object key is unavailable",
                                                      object->second.key_id));
            auto data = co_await options_.keys->data_key(object->second.key_id, false);
            if (!data)
                co_return std::unexpected(data.error());
            crypto_detail::wipe(*data);
        }
        co_return true;
    }
    auto found = index_.find(id);
    if (!found)
        co_return std::unexpected(found.error());
    co_return found->has_value();
}

runtime::task<blob_availability> store_backend::has(digest id) {
    auto local = co_await contains(id);
    if (!local) {
        co_return std::unexpected(local.error());
    }
    if (*local) {
        co_return blob_availability::local;
    }
    const auto remote = remotes_.find(to_string(id));
    if (remote == remotes_.end()) {
        co_return blob_availability::absent;
    }
    for (const auto& chunk : remote->second.chunks) {
        auto present = index_.find(chunk);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (!*present) {
            co_return blob_availability::remote;
        }
    }
    co_return blob_availability::local;
}

runtime::task<void>
store_backend::register_remote_source(std::string id, std::shared_ptr<remote_blob_source> source) {
    if (id.empty() || !source) {
        co_return std::unexpected(store_error(
            errc::invalid_argument, "invalid remote blob source", "store::register_remote_source"));
    }
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error) {
        co_return std::unexpected(
            store_error(errc::busy, lock_error.message(), "store::register_remote_source"));
    }
    remote_sources_.insert_or_assign(std::move(id), std::move(source));
    co_return result<void>{};
}

runtime::task<void> store_backend::register_remote(blob value) {
    if (!value.remote || value.remote->source.empty() ||
        (value.content.alg != algorithm::blake3 && value.content.alg != algorithm::sha256) ||
        value.chunks.empty() || value.chunks.size() != value.chunk_sizes.size()) {
        co_return std::unexpected(store_error(
            errc::invalid_argument, "invalid remote blob metadata", to_string(value.content)));
    }
    const auto total =
        std::accumulate(value.chunk_sizes.begin(), value.chunk_sizes.end(), std::uint64_t{});
    if (total != value.size || std::ranges::any_of(value.chunks, [](const digest& chunk) {
            return chunk.alg != algorithm::blake3 && chunk.alg != algorithm::sha256;
        })) {
        co_return std::unexpected(store_error(
            errc::invalid_argument, "invalid remote chunk metadata", to_string(value.content)));
    }
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error) {
        co_return std::unexpected(
            store_error(errc::busy, lock_error.message(), "store::register_remote"));
    }
    remotes_[to_string(value.content)] = std::move(value);
    co_return co_await save_remotes();
}

runtime::task<void> store_backend::evict(digest id) {
    co_return co_await collector(*this).evict(id);
}

runtime::task<std::uint64_t> store_backend::local_bytes() {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error) {
        co_return std::unexpected(
            store_error(errc::busy, lock_error.message(), "store::local_bytes"));
    }
    auto seen = std::set<std::string>{};
    auto bytes = std::uint64_t{};
    for (const auto& [unused, pack] : packs_) {
        for (const auto& entry : pack.entries) {
            const auto key = to_string(entry.id);
            if (seen.contains(key)) {
                continue;
            }
            auto location = index_.find(entry.id);
            if (!location) {
                co_return std::unexpected(location.error());
            }
            if (!*location) {
                continue;
            }
            const auto& active = **location;
            if (active.pack_id != entry.location.pack_id ||
                active.offset != entry.location.offset) {
                continue;
            }
            seen.insert(key);
            bytes += entry.raw_length;
        }
    }
    co_return bytes;
}

result<std::uint64_t> store_backend::remote_bytes(digest blob_record) const {
    const auto record = remote_records_.find(to_string(blob_record));
    if (record == remote_records_.end()) {
        return std::uint64_t{};
    }
    const auto remote = remotes_.find(to_string(record->second));
    if (remote == remotes_.end()) {
        return std::unexpected(store_error(
            errc::corrupt, "remote blob record has no location", to_string(blob_record)));
    }
    if (objects_.contains(to_string(remote->second.content))) {
        return std::uint64_t{};
    }
    auto bytes = std::uint64_t{};
    for (auto index = std::size_t{}; index < remote->second.chunks.size(); ++index) {
        auto present = index_.find(remote->second.chunks[index]);
        if (!present) {
            return std::unexpected(present.error());
        }
        if (!*present) {
            bytes += remote->second.chunk_sizes[index];
        }
    }
    return bytes;
}

std::vector<digest> store_backend::attestations(digest subject) const {
    return subjects_.find(subject);
}

std::vector<digest> store_backend::object_ids(std::string_view kind) const {
    auto output = std::vector<digest>{};
    for (const auto& [unused, object] : objects_) {
        static_cast<void>(unused);
        if (object.kind == kind) {
            output.push_back(object.id);
        }
    }
    return output;
}

runtime::task<std::optional<object_key_version>> store_backend::key_version(digest id) {
    const auto object = objects_.find(to_string(id));
    std::string reference;
    if (object != objects_.end()) {
        reference = object->second.key_id;
    } else if (const auto keyed_chunk = chunk_keys_.find(to_string(id));
               keyed_chunk != chunk_keys_.end()) {
        reference = keyed_chunk->second;
    } else {
        auto chunk = index_.find(id);
        if (!chunk)
            co_return std::unexpected(chunk.error());
        if (!*chunk) {
            co_return std::unexpected(
                store_error(errc::not_found, "object is not present", to_string(id)));
        }
    }
    if (reference.empty())
        co_return std::optional<object_key_version>{};
    auto parsed = identity_detail::parse_key_reference(reference);
    if (!parsed) {
        co_return std::unexpected(
            store_error(errc::corrupt, "object has an invalid key version", reference));
    }
    bool readable = false;
    if (options_.keys) {
        auto data = co_await options_.keys->data_key(reference, false);
        if (data) {
            crypto_detail::wipe(*data);
            readable = true;
        } else if (data.error().code != errc::unauthorized) {
            co_return std::unexpected(data.error());
        }
    }
    co_return object_key_version{std::move(parsed->key_id), parsed->version.value_or(1), readable};
}

runtime::task<std::shared_ptr<lease_state>> store_backend::lease_root(digest id,
                                                                      std::chrono::seconds ttl) {
    co_return co_await lease_root(id, ttl, {});
}

runtime::task<std::shared_ptr<lease_state>>
store_backend::lease_root(digest id, std::chrono::seconds ttl, std::string owner) {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error)
        co_return std::unexpected(
            store_error(errc::busy, lock_error.message(), "store::lease_root"));
    if (!objects_.contains(to_string(id))) {
        co_return std::unexpected(
            store_error(errc::not_found, "lease root is not present", to_string(id)));
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    if (ttl < std::chrono::seconds::zero() || ttl > std::chrono::sys_seconds::max() - now) {
        co_return std::unexpected(
            store_error(errc::invalid_argument, "lease ttl is invalid", "store::lease_root"));
    }
    co_return co_await create_lease_until(root_ / "leases", id, now + ttl, std::move(owner));
}

const std::filesystem::path& store_backend::root_path() const noexcept {
    return root_;
}

const store_options& store_backend::configuration() const noexcept {
    return options_;
}

runtime::task<collection_report> store_backend::collect(collection_options options) {
    co_return co_await collector(*this).run(options);
}

runtime::task<void> store_backend::redact(std::vector<digest> ids) {
    co_return co_await collector(*this).redact(ids);
}

} // namespace liquid::store_detail
