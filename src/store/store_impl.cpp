#include "store_impl.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <stdexcept>
#include <system_error>

#include "chunker.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> objects_magic{std::byte{'L'}, std::byte{'Q'}, std::byte{'O'},
                                                  std::byte{'B'}, std::byte{'J'}, std::byte{1},
                                                  std::byte{0}, std::byte{0}};

error store_error(errc code, std::string message, std::string context) {
    return {code, std::move(message), std::move(context)};
}
void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * i))});
}
void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * i))});
}
std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 4 > bytes.size()) return 0;
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) value |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + i])) << (8U * i);
    return value;
}
std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 8 > bytes.size()) return 0;
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + i])) << (8U * i);
    return value;
}
bool valid_algorithm(unsigned char value) {
    return value == static_cast<unsigned char>(algorithm::blake3) || value == static_cast<unsigned char>(algorithm::sha256);
}

}  // namespace

store_backend::store_backend(std::filesystem::path root, store_options options)
    : root_(std::move(root)), options_(options), index_(root_ / "index") {}

runtime::task<std::unique_ptr<store_backend>> store_backend::open(std::filesystem::path root,
                                                                   store_options options) {
    try {
        if (options.hash_algorithm != algorithm::blake3 && options.hash_algorithm != algorithm::sha256) {
            co_return std::unexpected(store_error(errc::invalid_argument, "unknown hash algorithm", root.string()));
        }
        std::filesystem::create_directories(root / "packs");
        std::filesystem::create_directories(root / "index");
        std::filesystem::create_directories(root / "tmp");
        const auto marker = root / "LIQUID_STORE";
        if (std::filesystem::exists(marker)) {
            auto text = co_await read_file(marker);
            if (!text) co_return std::unexpected(text.error());
            const std::string expected = "liquid-store 1\n";
            if (text->size() != expected.size() || std::memcmp(text->data(), expected.data(), expected.size()) != 0) {
                co_return std::unexpected(store_error(errc::unsupported, "unknown liquid store format", marker.string()));
            }
        } else {
            const std::string text = "liquid-store 1\n";
            std::vector<std::byte> bytes(reinterpret_cast<const std::byte*>(text.data()),
                                         reinterpret_cast<const std::byte*>(text.data() + text.size()));
            auto saved = co_await write_file(marker, std::move(bytes));
            if (!saved) co_return std::unexpected(saved.error());
        }
        auto result = std::unique_ptr<store_backend>(new store_backend(std::move(root), options));
        auto objects = co_await result->load_objects();
        if (!objects) co_return std::unexpected(objects.error());
        const bool manifest_exists = std::filesystem::exists(result->root_ / "index" / "MANIFEST");
        auto opened = co_await result->index_.open();
        if (!opened && opened.error().code != errc::corrupt) co_return std::unexpected(opened.error());
        auto indexed = co_await result->index_packs(!opened || !manifest_exists);
        if (!indexed) co_return std::unexpected(indexed.error());
        co_return result;
    } catch (const std::system_error& exception) {
        co_return std::unexpected(store_error(errc::io, exception.what(), root.string()));
    } catch (const std::exception& exception) {
        co_return std::unexpected(store_error(errc::internal, exception.what(), root.string()));
    }
}

runtime::task<void> store_backend::load_objects() {
    const auto path = root_ / "packs" / "objects.meta";
    if (!std::filesystem::exists(path)) co_return result<void>{};
    auto raw = co_await read_file(path);
    if (!raw) co_return std::unexpected(raw.error());
    const auto bytes = std::span<const std::byte>(*raw);
    if (bytes.size() < objects_magic.size() + 4 || !std::equal(objects_magic.begin(), objects_magic.end(), bytes.begin())) {
        co_return std::unexpected(store_error(errc::corrupt, "invalid object map", path.string()));
    }
    const auto count = get_u32(bytes, 8);
    std::size_t at = 12;
    for (std::uint32_t number = 0; number < count; ++number) {
        if (at + 45 > bytes.size() || !valid_algorithm(std::to_integer<unsigned char>(bytes[at]))) {
            co_return std::unexpected(store_error(errc::corrupt, "truncated object map", path.string()));
        }
        stored_object object{};
        object.id.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
        std::copy_n(bytes.begin() + at, 32, object.id.bytes.begin()); at += 32;
        object.size = get_u64(bytes, at); at += 8;
        const auto chunks = get_u32(bytes, at); at += 4;
        if (chunks > (bytes.size() - at) / 33) co_return std::unexpected(store_error(errc::corrupt, "truncated object chunks", path.string()));
        object.chunks.reserve(chunks);
        for (std::uint32_t chunk = 0; chunk < chunks; ++chunk) {
            digest id{};
            if (!valid_algorithm(std::to_integer<unsigned char>(bytes[at]))) co_return std::unexpected(store_error(errc::corrupt, "unknown object chunk algorithm", path.string()));
            id.alg = static_cast<algorithm>(std::to_integer<unsigned char>(bytes[at++]));
            std::copy_n(bytes.begin() + at, 32, id.bytes.begin()); at += 32;
            object.chunks.push_back(id);
        }
        objects_.emplace(to_string(object.id), std::move(object));
    }
    if (at != bytes.size()) co_return std::unexpected(store_error(errc::corrupt, "trailing object map data", path.string()));
    co_return result<void>{};
}

runtime::task<void> store_backend::save_objects() {
    std::vector<std::byte> bytes(objects_magic.begin(), objects_magic.end());
    put_u32(bytes, static_cast<std::uint32_t>(objects_.size()));
    for (const auto& [unused, object] : objects_) {
        bytes.push_back(std::byte{static_cast<unsigned char>(object.id.alg)});
        bytes.insert(bytes.end(), object.id.bytes.begin(), object.id.bytes.end());
        put_u64(bytes, object.size);
        put_u32(bytes, static_cast<std::uint32_t>(object.chunks.size()));
        for (const auto& chunk : object.chunks) {
            bytes.push_back(std::byte{static_cast<unsigned char>(chunk.alg)});
            bytes.insert(bytes.end(), chunk.bytes.begin(), chunk.bytes.end());
        }
    }
    co_return co_await write_file(root_ / "packs" / "objects.meta", std::move(bytes));
}

runtime::task<void> store_backend::index_packs(bool rebuild) {
    std::vector<index_entry> all;
    std::uint32_t highest = 0;
    try {
        for (const auto& file : std::filesystem::directory_iterator(root_ / "packs")) {
            const auto name = file.path().filename().string();
            if (!file.is_regular_file() || !name.starts_with("pack-") || !name.ends_with(".pack")) continue;
            const auto digits = name.substr(5, name.size() - 10);
            const auto id = static_cast<std::uint32_t>(std::stoul(digits));
            auto pack = co_await read_pack(file.path(), id);
            if (!pack) co_return std::unexpected(pack.error());
            highest = std::max(highest, id);
            for (const auto& entry : pack->entries) all.push_back({entry.id, entry.location});
            packs_.emplace(id, std::move(*pack));
        }
    } catch (const std::exception& exception) {
        co_return std::unexpected(store_error(errc::io, exception.what(), (root_ / "packs").string()));
    }
    current_pack_ = highest == 0 ? 1 : highest;
    if (!packs_.contains(current_pack_)) packs_.emplace(current_pack_, pack_contents{current_pack_, {}, 16});
    if (rebuild) {
        index_.set_highest_pack_id(highest);
        co_return co_await index_.rebuild(std::move(all));
    }
    for (auto& entry : all) {
        auto inserted = co_await index_.insert(std::move(entry));
        if (!inserted) co_return std::unexpected(inserted.error());
    }
    index_.set_highest_pack_id(highest);
    co_return co_await index_.flush();
}

runtime::task<pack_entry> store_backend::store_chunk(digest id, std::vector<std::byte> raw) {
    auto present = co_await index_.find(id);
    if (!present) co_return std::unexpected(present.error());
    if (*present) co_return pack_entry{id, **present, static_cast<std::uint32_t>(raw.size())};
    auto& active = packs_.at(current_pack_);
    const auto limit = options_.pack_size == 0 ? 64ULL * 1024ULL * 1024ULL : options_.pack_size;
    if (!active.entries.empty() && active.data_end + raw.size() > limit) {
        ++current_pack_;
        packs_.emplace(current_pack_, pack_contents{current_pack_, {}, 16});
    }
    auto& destination = packs_.at(current_pack_);
    auto written = co_await append_pack_entry(root_ / "packs" / ("pack-" + std::to_string(current_pack_) + ".pack"),
                                               destination, id, std::move(raw));
    if (!written) co_return std::unexpected(written.error());
    destination.entries.push_back(*written);
    destination.data_end += 49 + written->location.length;
    auto inserted = co_await index_.insert({id, written->location});
    if (!inserted) co_return std::unexpected(inserted.error());
    co_return *written;
}

runtime::task<std::vector<std::byte>> store_backend::load_chunk(digest id) {
    auto found = co_await index_.find(id);
    if (!found) co_return std::unexpected(found.error());
    if (!*found) co_return std::unexpected(store_error(errc::not_found, "chunk is not present", to_string(id)));
    co_return co_await read_pack_entry(root_ / "packs" / ("pack-" + std::to_string((*found)->pack_id) + ".pack"), **found, id);
}

runtime::task<void> store_backend::add_object(stored_object object) {
    objects_[to_string(object.id)] = std::move(object);
    co_return co_await save_objects();
}

runtime::task<digest> store_backend::put(std::vector<std::byte> input) {
    auto [lock_error, guard] = co_await mutation_.scoped_lock();
    if (lock_error) co_return std::unexpected(store_error(errc::busy, lock_error.message(), "store::put"));
    digest whole{};
    std::vector<chunk_range> ranges;
    if (input.size() > 4U * 1024U * 1024U) {
        auto hashed = co_await runtime::offload([input] {
            const auto output = hash(algorithm::blake3, input);
            if (!output) throw std::runtime_error(output.error().message);
            return *output;
        });
        if (!hashed) co_return std::unexpected(hashed.error());
        whole = *hashed;
        auto chunked = co_await runtime::offload([input] { return fastcdc(input); });
        if (!chunked) co_return std::unexpected(chunked.error());
        ranges = std::move(*chunked);
    } else {
        const auto hashed = hash(algorithm::blake3, input);
        if (!hashed) co_return std::unexpected(hashed.error());
        whole = *hashed;
        ranges = fastcdc(input);
    }
    if (objects_.contains(to_string(whole))) co_return whole;
    stored_object object{whole, input.size(), {}};
    for (const auto part : ranges) {
        const auto bytes = std::span<const std::byte>(input).subspan(part.offset, part.size);
        const auto id = hash(algorithm::blake3, bytes);
        if (!id) co_return std::unexpected(id.error());
        auto stored = co_await store_chunk(*id, std::vector<std::byte>(bytes.begin(), bytes.end()));
        if (!stored) co_return std::unexpected(stored.error());
        object.chunks.push_back(*id);
    }
    // Store the chunk list as normal content. The compact mapping associates the
    // full-stream digest with this descriptor and is rebuilt independently of the index.
    std::vector<std::byte> descriptor;
    descriptor.reserve(8 + object.chunks.size() * 33);
    put_u64(descriptor, object.size);
    for (const auto& id : object.chunks) {
        descriptor.push_back(std::byte{static_cast<unsigned char>(id.alg)});
        descriptor.insert(descriptor.end(), id.bytes.begin(), id.bytes.end());
    }
    const auto descriptor_id = hash(algorithm::blake3, descriptor);
    if (!descriptor_id) co_return std::unexpected(descriptor_id.error());
    auto descriptor_stored = co_await store_chunk(*descriptor_id, std::move(descriptor));
    if (!descriptor_stored) co_return std::unexpected(descriptor_stored.error());
    auto saved = co_await add_object(std::move(object));
    if (!saved) co_return std::unexpected(saved.error());
    index_.set_highest_pack_id(current_pack_);
    auto flushed = co_await index_.flush();
    if (!flushed) co_return std::unexpected(flushed.error());
    co_return whole;
}

runtime::task<std::vector<std::byte>> store_backend::get(digest id) {
    const auto it = objects_.find(to_string(id));
    if (it == objects_.end()) co_return std::unexpected(store_error(errc::not_found, "object is not present", to_string(id)));
    std::vector<std::byte> out;
    out.reserve(it->second.size);
    for (const auto& chunk : it->second.chunks) {
        auto raw = co_await load_chunk(chunk);
        if (!raw) co_return std::unexpected(raw.error());
        out.insert(out.end(), raw->begin(), raw->end());
    }
    if (out.size() != it->second.size) co_return std::unexpected(store_error(errc::corrupt, "object length differs from its chunk list", to_string(id)));
    const auto actual = hash(id.alg, out);
    if (!actual || actual->bytes != id.bytes) co_return std::unexpected(store_error(errc::corrupt, "object digest verification failed", to_string(id)));
    co_return out;
}

runtime::task<std::uint64_t> store_backend::read(digest id, std::uint64_t offset, std::span<std::byte> into) {
    const auto it = objects_.find(to_string(id));
    if (it == objects_.end()) co_return std::unexpected(store_error(errc::not_found, "object is not present", to_string(id)));
    if (offset >= it->second.size || into.empty()) co_return std::uint64_t{0};
    const auto wanted = std::min<std::uint64_t>(into.size(), it->second.size - offset);
    std::uint64_t position = 0, copied = 0;
    for (const auto& chunk : it->second.chunks) {
        auto raw = co_await load_chunk(chunk);
        if (!raw) co_return std::unexpected(raw.error());
        const auto end = position + raw->size();
        if (end > offset && copied < wanted) {
            const auto begin = static_cast<std::size_t>(std::max(position, offset) - position);
            const auto amount = static_cast<std::size_t>(std::min<std::uint64_t>(end - (position + begin), wanted - copied));
            std::copy_n(raw->begin() + begin, amount, into.begin() + copied);
            copied += amount;
        }
        position = end;
        if (copied == wanted) break;
    }
    co_return copied;
}

runtime::task<bool> store_backend::contains(digest id) {
    if (objects_.contains(to_string(id))) co_return true;
    auto found = co_await index_.find(id);
    if (!found) co_return std::unexpected(found.error());
    co_return found->has_value();
}

}  // namespace liquid::store_detail
