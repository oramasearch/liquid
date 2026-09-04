#include "store_impl.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "chunker.hpp"
#include "collector.hpp"
#include "faults.hpp"
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
    : root_(std::move(root)), options_(std::move(options)), index_(root_ / "index") {}

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
        const bool manifest_exists = std::filesystem::exists(result->root_ / "index" / "MANIFEST");
        auto opened = co_await result->index_.open();
        if (!opened && opened.error().code != errc::corrupt)
            co_return std::unexpected(opened.error());
        auto collection = co_await load_collection_manifest(result->root_);
        if (!collection)
            co_return std::unexpected(collection.error());
        auto indexed = co_await result->index_packs(
            !opened || !manifest_exists || result->index_.collection_epoch() != collection->epoch);
        if (!indexed)
            co_return std::unexpected(indexed.error());
        co_return result;
    } catch (const std::system_error& caught) {
        co_return std::unexpected(store_error(errc::io, caught.what(), root.string()));
    } catch (const std::exception& caught) {
        co_return std::unexpected(store_error(errc::internal, caught.what(), root.string()));
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
    if ((!version_one && !version_two && !version_three) || bytes.size() < 12)
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
        if (version_two || version_three) {
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
        if (version_three) {
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
        if (chunks > (bytes.size() - at) / 33)
            co_return std::unexpected(
                store_error(errc::corrupt, "truncated object chunks", path.string()));
        object.chunks.reserve(chunks);
        for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
            digest id{};
            if (!valid_algorithm(std::to_integer<unsigned char>(bytes[at])))
                co_return std::unexpected(
                    store_error(errc::corrupt, "unknown object chunk algorithm", path.string()));
            id.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
            std::copy_n(bytes.begin() + at, 32, id.bytes.begin());
            at += 32;
            object.chunks.push_back(id);
        }
        objects_.emplace(to_string(object.id), std::move(object));
    }
    if (at != bytes.size())
        co_return std::unexpected(
            store_error(errc::corrupt, "trailing object map data", path.string()));
    co_return result<void>{};
}

runtime::task<void> store_backend::save_objects() {
    auto staged = co_await stage_objects();
    if (!staged)
        co_return std::unexpected(staged.error());
    co_return co_await publish_staged_objects();
}

runtime::task<void> store_backend::stage_objects() {
    std::vector<std::byte> bytes(objects_magic_v3.begin(), objects_magic_v3.end());
    put_u32(bytes, static_cast<std::uint32_t>(objects_.size()));
    for (const auto& [unused, object] : objects_) {
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
        for (const auto& chunk : object.chunks) {
            bytes.push_back(std::byte{static_cast<unsigned char>(chunk.alg)});
            bytes.insert(bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
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
    objects_[to_string(object.id)] = std::move(object);
    co_return co_await save_objects();
}

runtime::task<digest> store_backend::put(std::vector<std::byte> input) {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error)
        co_return std::unexpected(store_error(errc::busy, lock_error.message(), "store::put"));
    std::optional<crypto_detail::domain_keys> encryption;
    std::string key_id;
    if (options_.keys) {
        if (options_.write_key.empty()) {
            auto default_key = options_.keys->default_key_id();
            if (!default_key)
                co_return std::unexpected(default_key.error());
            key_id = std::move(*default_key);
        } else {
            key_id = options_.write_key;
        }
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
    std::vector<chunk_range> ranges;
    if (!encryption && input.size() > 4U * 1024U * 1024U) {
        const auto algorithm_value = options_.hash_algorithm;
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
                                       : hash(options_.hash_algorithm, input);
        if (!hashed)
            co_return std::unexpected(hashed.error());
        whole = *hashed;
        ranges = fastcdc(input);
    }
    if (objects_.contains(to_string(whole)))
        co_return whole;
    stored_object object;
    object.id = whole;
    object.size = input.size();
    object.key_id = key_id;
    object.kind = detect_object_kind(input);
    for (const auto part : ranges) {
        const auto bytes = std::span<const std::byte>(input).subspan(part.offset, part.size);
        const auto id = encryption ? crypto_detail::keyed_digest(encryption->chunk_mac, bytes)
                                   : hash(options_.hash_algorithm, bytes);
        if (!id)
            co_return std::unexpected(id.error());
        auto stored = co_await store_chunk(
            *id, std::vector<std::byte>(bytes.begin(), bytes.end()), encryption);
        if (!stored)
            co_return std::unexpected(stored.error());
        object.chunks.push_back(*id);
    }
    auto descriptor = encode_chunk_list(object.size, key_id, object.chunks);
    const auto descriptor_id = encryption
                                   ? crypto_detail::keyed_digest(encryption->chunk_mac, descriptor)
                                   : hash(options_.hash_algorithm, descriptor);
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
    index_.set_highest_pack_id(descriptor_stored->location.pack_id);
    auto flushed = co_await index_.flush();
    if (!flushed)
        co_return std::unexpected(flushed.error());
    co_return whole;
}

runtime::task<std::vector<std::byte>> store_backend::get(digest id) {
    const auto it = objects_.find(to_string(id));
    if (it == objects_.end())
        co_return std::unexpected(
            store_error(errc::not_found, "object is not present", to_string(id)));
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
    if (it == objects_.end())
        co_return std::unexpected(
            store_error(errc::not_found, "object is not present", to_string(id)));
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
    for (const auto& chunk : it->second.chunks) {
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

runtime::task<std::shared_ptr<lease_state>> store_backend::lease_root(digest id,
                                                                      std::chrono::seconds ttl) {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error)
        co_return std::unexpected(
            store_error(errc::busy, lock_error.message(), "store::lease_root"));
    if (!objects_.contains(to_string(id))) {
        co_return std::unexpected(
            store_error(errc::not_found, "lease root is not present", to_string(id)));
    }
    co_return co_await create_lease(root_ / "leases", id, ttl);
}

runtime::task<collection_report> store_backend::collect(collection_options options) {
    co_return co_await collector(*this).run(options);
}

} // namespace liquid::store_detail
