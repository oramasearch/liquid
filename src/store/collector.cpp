#include "collector.hpp"
#include "compactor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <deque>
#include <set>
#include <span>
#include <system_error>
#include <utility>

#include "faults.hpp"
#include "leases.hpp"
#include "pack.hpp"
#include "references.hpp"
#include "store_impl.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> manifest_magic{std::byte{'L'},
                                                  std::byte{'Q'},
                                                  std::byte{'G'},
                                                  std::byte{'C'},
                                                  std::byte{'T'},
                                                  std::byte{'M'},
                                                  std::byte{1},
                                                  std::byte{0}};

error collection_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (8U * index))});
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

std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

bool marked(const std::vector<std::byte>& bitmap, std::size_t frame) {
    return frame / 8 < bitmap.size() &&
           (std::to_integer<unsigned char>(bitmap[frame / 8]) & (1U << (frame % 8))) != 0;
}

void mark(std::vector<std::byte>& bitmap, std::size_t frame) {
    if (bitmap.size() <= frame / 8)
        bitmap.resize(frame / 8 + 1);
    bitmap[frame / 8] |= std::byte{static_cast<unsigned char>(1U << (frame % 8))};
}

std::vector<std::byte> encode_manifest(const collection_manifest& manifest) {
    std::vector<std::byte> bytes(manifest_magic.begin(), manifest_magic.end());
    put_u64(bytes, manifest.epoch);
    put_u32(bytes, static_cast<std::uint32_t>(manifest.tombstones.size()));
    for (const auto& [pack_id, bitmap] : manifest.tombstones) {
        put_u32(bytes, pack_id);
        put_u32(bytes, static_cast<std::uint32_t>(bitmap.size()));
        bytes.insert(bytes.end(), bitmap.begin(), bitmap.end());
    }
    put_u64(bytes, checksum(bytes));
    return bytes;
}

runtime::task<void> save_manifest(const std::filesystem::path& root,
                                  const collection_manifest& manifest) {
    const auto directory = root / "gc";
    const auto temporary = directory / "MANIFEST.tmp";
    const auto destination = directory / "MANIFEST";
    auto written = co_await write_file(temporary, encode_manifest(manifest));
    if (!written)
        co_return std::unexpected(written.error());
    try {
        visit_fault({fault_operation::rename, fault_phase::before, temporary});
        std::filesystem::rename(temporary, destination);
        visit_fault({fault_operation::rename, fault_phase::after, destination});
        co_return result<void>{};
    } catch (const std::system_error& caught) {
        co_return std::unexpected(collection_error(errc::io, caught.what(), destination));
    }
}

bool after_high_water(std::uint32_t high_water_pack,
                      std::uint64_t high_water_offset,
                      const pack_entry& entry) {
    return entry.location.pack_id > high_water_pack || (entry.location.pack_id == high_water_pack &&
                                                        entry.location.offset >= high_water_offset);
}

std::string root_reason(const lease_record& lease) {
    return "lease " + lease.id + " for root " + to_string(lease.root);
}

} // namespace

runtime::task<void> save_collection_manifest(const std::filesystem::path& root,
                                             const collection_manifest& manifest) {
    co_return co_await save_manifest(root, manifest);
}

bool collection_manifest::dead(std::uint32_t pack_id, std::size_t frame) const noexcept {
    const auto pack = tombstones.find(pack_id);
    return pack != tombstones.end() && marked(pack->second, frame);
}

runtime::task<collection_manifest> load_collection_manifest(std::filesystem::path root) {
    const auto path = std::move(root) / "gc" / "MANIFEST";
    if (!std::filesystem::exists(path))
        co_return collection_manifest{};
    auto raw = co_await read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    const auto bytes = std::span<const std::byte>(*raw);
    if (bytes.size() < 24 ||
        !std::equal(manifest_magic.begin(), manifest_magic.end(), bytes.begin()) ||
        checksum(bytes.first(bytes.size() - 8)) != get_u64(bytes, bytes.size() - 8)) {
        co_return std::unexpected(
            collection_error(errc::corrupt, "invalid collection manifest", path));
    }
    collection_manifest manifest;
    manifest.epoch = get_u64(bytes, 8);
    const auto count = get_u32(bytes, 16);
    std::size_t at = 20;
    for (std::uint32_t number = 0; number != count; ++number) {
        if (at + 8 > bytes.size() - 8)
            co_return std::unexpected(
                collection_error(errc::corrupt, "truncated collection manifest", path));
        const auto pack_id = get_u32(bytes, at);
        const auto size = get_u32(bytes, at + 4);
        at += 8;
        if (at + size > bytes.size() - 8)
            co_return std::unexpected(
                collection_error(errc::corrupt, "truncated collection bitmap", path));
        manifest.tombstones.emplace(
            pack_id, std::vector<std::byte>(bytes.begin() + at, bytes.begin() + at + size));
        at += size;
    }
    if (at != bytes.size() - 8)
        co_return std::unexpected(
            collection_error(errc::corrupt, "trailing collection manifest data", path));
    co_return manifest;
}

runtime::task<collection_report> collector::run(collection_options options) {
    if (options.min_lease_age < std::chrono::seconds::zero()) {
        co_return std::unexpected(error{
            errc::invalid_argument, "minimum lease age cannot be negative", "store::collect"});
    }
    if (!std::isfinite(options.compact_below) || options.compact_below < 0.0 ||
        options.compact_below > 1.0) {
        co_return std::unexpected(error{errc::invalid_argument,
                                        "compaction threshold must be between zero and one",
                                        "store::collect"});
    }

    auto previous = co_await load_collection_manifest(store_.root_);
    if (!previous)
        co_return std::unexpected(previous.error());
    if (options.compact_only) {
        collection_report report;
        report.epoch = previous->epoch;
        auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
        if (lock_error) {
            co_return std::unexpected(
                error{errc::busy, lock_error.message(), "store::collect compaction"});
        }
        compactor compact(store_);
        auto compacted = co_await compact.run(options, *previous, report);
        if (!compacted)
            co_return std::unexpected(compacted.error());
        co_return report;
    }
    collection_manifest committed;
    committed.epoch = previous->epoch + 1;
    committed.tombstones = previous->tombstones;
    std::uint32_t high_water_pack{};
    std::uint64_t high_water_offset{};
    std::map<std::uint32_t, std::vector<std::byte>> epoch_marks;

    std::map<std::string, stored_object> snapshot_objects;
    std::map<std::uint32_t, pack_contents> snapshot_packs;
    {
        auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
        if (lock_error)
            co_return std::unexpected(
                error{errc::busy, lock_error.message(), "store::collect snapshot"});
        snapshot_objects = store_.objects_;
        snapshot_packs = store_.packs_;
        high_water_pack = store_.current_pack_;
        high_water_offset = store_.packs_.at(store_.current_pack_).data_end;
    }

    auto leases = co_await load_leases(store_.root_ / "leases");
    if (!leases)
        co_return std::unexpected(leases.error());
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::vector<lease_record> live_leases;
    for (const auto& lease : *leases) {
        if (lease.expiry + options.min_lease_age > now)
            live_leases.push_back(lease);
    }

    collection_report report;
    report.epoch = committed.epoch;
    report.roots = live_leases.size();
    std::set<std::string> live_objects;
    std::set<std::string> live_chunks;
    std::map<std::string, std::string> reasons;
    std::deque<std::pair<digest, std::string>> pending;
    for (const auto& lease : live_leases)
        pending.emplace_back(lease.root, root_reason(lease));

    reference_registry registry;
    std::set<std::string> unknown_kinds;
    std::map<std::string, chunk_location> snapshot_locations;
    for (const auto& [unused, pack] : snapshot_packs) {
        for (const auto& entry : pack.entries)
            snapshot_locations.try_emplace(to_string(entry.id), entry.location);
    }
    while (!pending.empty()) {
        auto [id, reason] = std::move(pending.front());
        pending.pop_front();
        const auto object_key = to_string(id);
        if (!live_objects.insert(object_key).second)
            continue;
        const auto object = snapshot_objects.find(object_key);
        if (object == snapshot_objects.end()) {
            if (snapshot_locations.contains(object_key)) {
                live_chunks.insert(object_key);
                reasons.try_emplace(object_key, reason);
            }
            continue;
        }
        auto descriptor =
            encode_chunk_list(object->second.size, object->second.key_id, object->second.chunks);
        const auto chunk_references = registry.extract("chunk-list", descriptor);
        for (const auto& chunk : chunk_references.digests) {
            live_chunks.insert(to_string(chunk));
            reasons.try_emplace(to_string(chunk), reason);
        }
        if (object->second.has_descriptor) {
            const auto descriptor_key = to_string(object->second.descriptor);
            live_chunks.insert(descriptor_key);
            reasons.try_emplace(descriptor_key, reason + " (chunk list)");
        }

        auto references = registry.extract(object->second.kind, {});
        if (!references.known) {
            unknown_kinds.insert(object->second.kind);
        } else {
            auto object_bytes =
                co_await store_.load_snapshot_object(object->second, snapshot_locations);
            if (!object_bytes)
                co_return std::unexpected(object_bytes.error());
            references = registry.extract(object->second.kind, *object_bytes);
        }
        if (references.known && object->second.kind == "chunk-list") {
            for (const auto& chunk : references.digests) {
                live_chunks.insert(to_string(chunk));
                reasons.try_emplace(to_string(chunk), reason);
            }
        } else if (references.known) {
            for (const auto& reference : references.digests)
                pending.emplace_back(reference, reason + " -> " + object->second.kind);
        }
    }
    report.live_objects = live_objects.size();
    report.unknown_kinds.assign(unknown_kinds.begin(), unknown_kinds.end());

    for (const auto& [pack_id, pack] : snapshot_packs) {
        auto& bitmap = epoch_marks[pack_id];
        bitmap.resize((pack.entries.size() + 7) / 8);
        for (std::size_t frame = 0; frame != pack.entries.size(); ++frame) {
            const auto& entry = pack.entries[frame];
            if (after_high_water(high_water_pack, high_water_offset, entry) ||
                live_chunks.contains(to_string(entry.id)))
                mark(bitmap, frame);
        }
        auto [yield_error] = co_await runtime::delay(std::chrono::nanoseconds{1});
        if (yield_error)
            co_return std::unexpected(
                error{errc::internal, yield_error.message(), "store::collect"});
    }

    auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
    if (lock_error)
        co_return std::unexpected(error{errc::busy, lock_error.message(), "store::collect sweep"});

    // A root or object created while marking is conservatively retained. Objects
    // written after the high-water mark are live by the epoch rule.
    auto current_leases = co_await load_leases(store_.root_ / "leases");
    if (!current_leases)
        co_return std::unexpected(current_leases.error());
    for (const auto& [unused, pack] : store_.packs_) {
        for (const auto& entry : pack.entries)
            snapshot_locations.insert_or_assign(to_string(entry.id), entry.location);
    }
    std::deque<std::pair<digest, std::string>> current_pending;
    for (const auto& lease : *current_leases) {
        if (lease.expiry + options.min_lease_age <= now)
            continue;
        if (live_objects.contains(to_string(lease.root)))
            continue;
        current_pending.emplace_back(lease.root, root_reason(lease));
    }
    while (!current_pending.empty()) {
        auto [id, reason] = std::move(current_pending.front());
        current_pending.pop_front();
        const auto object_key = to_string(id);
        if (!live_objects.insert(object_key).second)
            continue;
        const auto object = store_.objects_.find(object_key);
        if (object == store_.objects_.end()) {
            if (snapshot_locations.contains(object_key)) {
                live_chunks.insert(object_key);
                reasons.try_emplace(object_key, reason);
            }
            continue;
        }
        for (const auto& chunk : object->second.chunks) {
            live_chunks.insert(to_string(chunk));
            reasons.try_emplace(to_string(chunk), reason);
        }
        if (object->second.has_descriptor) {
            live_chunks.insert(to_string(object->second.descriptor));
            reasons.try_emplace(to_string(object->second.descriptor), reason + " (chunk list)");
        }
        auto references = registry.extract(object->second.kind, {});
        if (!references.known) {
            unknown_kinds.insert(object->second.kind);
            continue;
        }
        auto object_bytes =
            co_await store_.load_snapshot_object(object->second, snapshot_locations);
        if (!object_bytes)
            co_return std::unexpected(object_bytes.error());
        references = registry.extract(object->second.kind, *object_bytes);
        if (object->second.kind == "chunk-list") {
            for (const auto& chunk : references.digests) {
                live_chunks.insert(to_string(chunk));
                reasons.try_emplace(to_string(chunk), reason);
            }
        } else {
            for (const auto& reference : references.digests)
                current_pending.emplace_back(reference, reason + " -> " + object->second.kind);
        }
    }
    for (const auto& [key, object] : store_.objects_) {
        if (!snapshot_objects.contains(key)) {
            live_objects.insert(key);
            for (const auto& chunk : object.chunks) {
                live_chunks.insert(to_string(chunk));
                reasons.try_emplace(to_string(chunk), "written after epoch high-water mark");
            }
            if (object.has_descriptor) {
                live_chunks.insert(to_string(object.descriptor));
                reasons.try_emplace(to_string(object.descriptor),
                                    "written after epoch high-water mark");
            }
        }
    }

    std::vector<index_entry> retained_index;
    std::vector<std::uint32_t> delete_packs;
    for (const auto& [pack_id, pack] : store_.packs_) {
        auto& bitmap = epoch_marks[pack_id];
        bitmap.clear();
        bitmap.resize((pack.entries.size() + 7) / 8);
        std::uint64_t total_bytes = 0;
        std::uint64_t live_bytes = 0;
        std::uint64_t dead_bytes = 0;
        for (std::size_t frame = 0; frame != pack.entries.size(); ++frame) {
            const auto& entry = pack.entries[frame];
            const auto frame_bytes = std::uint64_t{49} + entry.location.length;
            total_bytes += frame_bytes;
            const auto is_live = after_high_water(high_water_pack, high_water_offset, entry) ||
                                 live_chunks.contains(to_string(entry.id));
            if (is_live) {
                mark(bitmap, frame);
                live_bytes += frame_bytes;
                retained_index.push_back({entry.id, entry.location});
                if (after_high_water(high_water_pack, high_water_offset, entry)) {
                    live_chunks.insert(to_string(entry.id));
                    reasons.try_emplace(to_string(entry.id), "written after epoch high-water mark");
                }
            } else {
                assert(!after_high_water(high_water_pack, high_water_offset, entry));
                mark(committed.tombstones[pack_id], frame);
                ++report.dead_chunks;
                dead_bytes += frame_bytes;
            }
        }
        if (total_bytes != 0 && live_bytes == 0) {
            delete_packs.push_back(pack_id);
            std::error_code size_error;
            const auto physical = std::filesystem::file_size(
                store_.root_ / "packs" / ("pack-" + std::to_string(pack_id) + ".pack"), size_error);
            if (!size_error)
                report.bytes_reclaimed += physical;
        } else if (dead_bytes != 0) {
            report.bytes_reclaimable_in_partially_dead_packs += dead_bytes;
            report.partially_dead_packs.push_back(
                {pack_id, total_bytes == 0 ? 1.0 : static_cast<double>(live_bytes) / total_bytes});
        }
    }
    report.live_objects = live_objects.size();
    report.live_chunks = live_chunks.size();
    report.packs_deleted = delete_packs.size();
    const bool current_pack_deleted =
        std::find(delete_packs.begin(), delete_packs.end(), store_.current_pack_) !=
        delete_packs.end();
    const auto next_pack_id = current_pack_deleted
                                  ? std::max(store_.current_pack_ + 1, high_water_pack + 1)
                                  : store_.current_pack_;
    if (options.explain) {
        for (const auto& key : live_chunks) {
            auto parsed = parse_digest(key);
            if (parsed)
                report.retained_chunks.push_back({*parsed, reasons[key]});
        }
    }
    if (options.dry_run)
        co_return report;

    for (auto iterator = store_.objects_.begin(); iterator != store_.objects_.end();) {
        if (!live_objects.contains(iterator->first))
            iterator = store_.objects_.erase(iterator);
        else
            ++iterator;
    }
    auto objects_staged = co_await store_.stage_objects();
    if (!objects_staged)
        co_return std::unexpected(objects_staged.error());
    auto manifest_saved = co_await save_collection_manifest(store_.root_, committed);
    if (!manifest_saved)
        co_return std::unexpected(manifest_saved.error());
    auto objects_saved = co_await store_.publish_staged_objects();
    if (!objects_saved)
        co_return std::unexpected(objects_saved.error());

    store_.index_.set_highest_pack_id(
        std::max({store_.index_.highest_pack_id(), high_water_pack, next_pack_id}));
    store_.index_.set_collection_epoch(committed.epoch);
    auto rebuilt = co_await store_.index_.rebuild(std::move(retained_index));
    if (!rebuilt)
        co_return std::unexpected(rebuilt.error());

    for (const auto pack_id : delete_packs) {
        const auto path = store_.root_ / "packs" / ("pack-" + std::to_string(pack_id) + ".pack");
        try {
            visit_fault({fault_operation::remove, fault_phase::before, path});
            std::filesystem::remove(path);
            visit_fault({fault_operation::remove, fault_phase::after, path});
            store_.packs_.erase(pack_id);
        } catch (const std::system_error& caught) {
            co_return std::unexpected(collection_error(errc::io, caught.what(), path));
        }
    }
    if (current_pack_deleted) {
        store_.current_pack_ = next_pack_id;
        store_.packs_.emplace(store_.current_pack_, pack_contents{store_.current_pack_, {}, 16});
    }
    compactor compact(store_);
    auto compacted = co_await compact.run(options, committed, report);
    if (!compacted)
        co_return std::unexpected(compacted.error());
    co_return report;
}

} // namespace liquid::store_detail
