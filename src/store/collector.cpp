#include "collector.hpp"
#include "compactor.hpp"

#include <algorithm>
#include <array>
#include <cassert>
#include <chrono>
#include <cmath>
#include <deque>
#include <limits>
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
    auto reason = "lease " + lease.id;
    const auto retention = lease.owner.find("retention:");
    if (retention != std::string::npos && (retention == 0 || lease.owner[retention - 1] == '\n')) {
        const auto first = lease.owner.find('\n', retention);
        const auto second =
            first == std::string::npos ? std::string::npos : lease.owner.find('\n', first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            reason += " from rule " + lease.owner.substr(first + 1, second - first - 1);
        }
    }
    if (lease.expiry == std::chrono::sys_seconds::max()) {
        reason += " until forever";
    } else {
        reason += " until " + std::to_string(lease.expiry.time_since_epoch().count());
    }
    return reason + " for root " + to_string(lease.root);
}

bool lease_is_live(const lease_record& lease,
                   std::chrono::sys_seconds now,
                   std::chrono::seconds grace) {
    if (lease.expiry == std::chrono::sys_seconds::max() || lease.expiry > now) {
        return true;
    }
    return now - lease.expiry < grace;
}

std::optional<std::string> retention_rule(const lease_record& lease) {
    const auto retention = lease.owner.find("retention:");
    if (retention == std::string::npos || (retention != 0 && lease.owner[retention - 1] != '\n')) {
        return std::nullopt;
    }
    const auto first = lease.owner.find('\n', retention);
    const auto second =
        first == std::string::npos ? std::string::npos : lease.owner.find('\n', first + 1);
    if (first == std::string::npos || second == std::string::npos) {
        return std::nullopt;
    }
    return lease.owner.substr(first + 1, second - first - 1);
}

void retain_delta_bases(const std::map<std::string, digest>& bases,
                        std::set<std::string>& chunks,
                        std::map<std::string, std::string>* reasons = nullptr) {
    auto pending = std::deque<std::string>{chunks.begin(), chunks.end()};
    while (!pending.empty()) {
        auto target = std::move(pending.front());
        pending.pop_front();
        const auto found = bases.find(target);
        if (found == bases.end()) {
            continue;
        }
        const auto base = to_string(found->second);
        if (!chunks.insert(base).second) {
            continue;
        }
        if (reasons) {
            const auto target_reason = reasons->find(target);
            reasons->try_emplace(base,
                                 target_reason == reasons->end()
                                     ? "delta base"
                                     : target_reason->second + " (delta base)");
        }
        pending.push_back(base);
    }
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

    if (options.compact_only) {
        auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
        if (lock_error) {
            co_return std::unexpected(
                error{errc::busy, lock_error.message(), "store::collect compaction"});
        }
        auto committed = co_await load_collection_manifest(store_.root_);
        if (!committed) {
            co_return std::unexpected(committed.error());
        }
        collection_report report;
        report.epoch = committed->epoch;
        compactor compact(store_);
        auto compacted = co_await compact.run(options, *committed, report);
        if (!compacted)
            co_return std::unexpected(compacted.error());
        co_return report;
    }
    collection_manifest committed;
    std::uint32_t high_water_pack{};
    std::uint64_t high_water_offset{};
    std::map<std::uint32_t, std::vector<std::byte>> epoch_marks;

    std::map<std::string, stored_object> snapshot_objects;
    std::map<std::uint32_t, pack_contents> snapshot_packs;
    std::map<std::string, digest> snapshot_bases;
    {
        auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
        if (lock_error)
            co_return std::unexpected(
                error{errc::busy, lock_error.message(), "store::collect snapshot"});
        snapshot_objects = store_.objects_;
        snapshot_packs = store_.packs_;
        snapshot_bases = store_.chunk_bases_;
        high_water_pack = store_.current_pack_;
        high_water_offset = store_.packs_.at(store_.current_pack_).data_end;
    }

    auto leases = co_await load_leases(store_.root_ / "leases");
    if (!leases)
        co_return std::unexpected(leases.error());
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    std::vector<lease_record> live_leases;
    for (const auto& lease : *leases) {
        if (lease_is_live(lease, now, options.min_lease_age))
            live_leases.push_back(lease);
    }

    collection_report report;
    report.roots = live_leases.size();
    std::set<std::string> live_objects;
    std::set<std::string> live_chunks;
    std::map<std::string, std::string> reasons;
    std::map<std::string, std::string> object_reasons;
    std::deque<std::pair<digest, std::string>> pending;
    for (const auto& lease : live_leases) {
        pending.emplace_back(lease.root, root_reason(lease));
        if (const auto rule = retention_rule(lease)) {
            ++report.retained_by_rule[*rule];
        }
    }

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
        object_reasons.try_emplace(object_key, reason);
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
    retain_delta_bases(snapshot_bases, live_chunks, &reasons);

    for (const auto& [pack_id, pack] : snapshot_packs) {
        auto& bitmap = epoch_marks[pack_id];
        bitmap.resize((pack.entries.size() + 7) / 8);
        for (std::size_t frame = 0; frame != pack.entries.size(); ++frame) {
            const auto& entry = pack.entries[frame];
            if (after_high_water(high_water_pack, high_water_offset, entry) ||
                live_chunks.contains(to_string(entry.id)))
                mark(bitmap, frame);
        }
        co_await runtime::yield();
    }

    auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
    if (lock_error)
        co_return std::unexpected(error{errc::busy, lock_error.message(), "store::collect sweep"});
    auto latest = co_await load_collection_manifest(store_.root_);
    if (!latest) {
        co_return std::unexpected(latest.error());
    }
    if (latest->epoch == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::unexpected(
            collection_error(errc::io, "collection epoch space exhausted", store_.root_ / "gc"));
    }
    committed = std::move(*latest);
    ++committed.epoch;
    report.epoch = committed.epoch;

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
        if (!lease_is_live(lease, now, options.min_lease_age))
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
        object_reasons.try_emplace(object_key, reason);
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
            object_reasons.try_emplace(key, "written after epoch high-water mark");
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
    for (const auto& [unused, pack] : store_.packs_) {
        static_cast<void>(unused);
        for (const auto& entry : pack.entries) {
            if (after_high_water(high_water_pack, high_water_offset, entry)) {
                live_chunks.insert(to_string(entry.id));
                reasons.try_emplace(to_string(entry.id), "written after epoch high-water mark");
            }
        }
    }
    retain_delta_bases(store_.chunk_bases_, live_chunks, &reasons);

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
            const auto was_dead = committed.dead(pack_id, frame);
            const auto is_live =
                !was_dead && (after_high_water(high_water_pack, high_water_offset, entry) ||
                              live_chunks.contains(to_string(entry.id)));
            if (is_live) {
                mark(bitmap, frame);
                live_bytes += frame_bytes;
                retained_index.push_back({entry.id, entry.location});
                if (after_high_water(high_water_pack, high_water_offset, entry)) {
                    live_chunks.insert(to_string(entry.id));
                    reasons.try_emplace(to_string(entry.id), "written after epoch high-water mark");
                }
            } else if (!was_dead) {
                assert(!after_high_water(high_water_pack, high_water_offset, entry));
                mark(committed.tombstones[pack_id], frame);
                ++report.dead_chunks;
                dead_bytes += frame_bytes;
            } else {
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
        for (const auto& key : live_objects) {
            auto parsed = parse_digest(key);
            if (parsed) {
                report.retained_objects.push_back({*parsed, object_reasons[key]});
            }
        }
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

runtime::task<void> collector::remove_local(collection_manifest manifest,
                                            std::set<std::string> target_objects,
                                            std::set<std::string> target_chunks,
                                            std::optional<std::string> excluded_remote,
                                            bool compact,
                                            bool refresh_subjects) {
    auto changed = false;
    for (const auto& key : target_objects) {
        changed = store_.objects_.erase(key) != 0 || changed;
    }
    auto retained_chunks = std::set<std::string>{};
    for (const auto& [unused, object] : store_.objects_) {
        for (const auto& chunk : object.chunks) {
            retained_chunks.insert(to_string(chunk));
        }
        if (object.has_descriptor) {
            retained_chunks.insert(to_string(object.descriptor));
        }
    }
    if (excluded_remote) {
        for (const auto& [key, remote] : store_.remotes_) {
            if (key == *excluded_remote) {
                continue;
            }
            for (const auto& chunk : remote.chunks) {
                retained_chunks.insert(to_string(chunk));
            }
        }
    }
    retain_delta_bases(store_.chunk_bases_, retained_chunks);

    auto report = collection_report{};
    auto retained_index = std::vector<index_entry>{};
    for (const auto& [pack_id, pack] : store_.packs_) {
        auto pack_changed = false;
        auto total_bytes = std::uint64_t{};
        auto live_bytes = std::uint64_t{};
        for (auto frame = std::size_t{}; frame < pack.entries.size(); ++frame) {
            const auto& entry = pack.entries[frame];
            const auto frame_bytes = std::uint64_t{49} + entry.location.length;
            total_bytes += frame_bytes;
            const auto key = to_string(entry.id);
            if (target_chunks.contains(key) && !retained_chunks.contains(key) &&
                !manifest.dead(pack_id, frame)) {
                mark(manifest.tombstones[pack_id], frame);
                pack_changed = true;
                changed = true;
            }
            if (!manifest.dead(pack_id, frame)) {
                live_bytes += frame_bytes;
                retained_index.push_back({entry.id, entry.location});
            }
        }
        if (pack_changed && total_bytes != 0) {
            report.partially_dead_packs.push_back(
                {pack_id, static_cast<double>(live_bytes) / static_cast<double>(total_bytes)});
        }
    }
    if (!changed) {
        co_return result<void>{};
    }

    ++manifest.epoch;
    report.epoch = manifest.epoch;
    auto objects_staged = co_await store_.stage_objects();
    if (!objects_staged) {
        co_return std::unexpected(objects_staged.error());
    }
    auto manifest_saved = co_await save_collection_manifest(store_.root_, manifest);
    if (!manifest_saved) {
        co_return std::unexpected(manifest_saved.error());
    }
    auto objects_saved = co_await store_.publish_staged_objects();
    if (!objects_saved) {
        co_return std::unexpected(objects_saved.error());
    }
    store_.index_.set_collection_epoch(manifest.epoch);
    auto rebuilt = co_await store_.index_.rebuild(std::move(retained_index));
    if (!rebuilt) {
        co_return std::unexpected(rebuilt.error());
    }
    if (refresh_subjects) {
        auto subjects = co_await store_.rebuild_subject_index();
        if (!subjects) {
            co_return std::unexpected(subjects.error());
        }
    }
    if (compact) {
        auto options = collection_options{};
        options.compact_below = 1.0;
        compactor pack_compactor(store_);
        auto compacted = co_await pack_compactor.run(options, manifest, report);
        if (!compacted) {
            co_return std::unexpected(compacted.error());
        }
    }
    co_return result<void>{};
}

runtime::task<void> collector::redact(std::span<const digest> ids) {
    auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
    if (lock_error) {
        co_return std::unexpected(error{errc::busy, lock_error.message(), "store::redact"});
    }
    auto manifest = co_await load_collection_manifest(store_.root_);
    if (!manifest) {
        co_return std::unexpected(manifest.error());
    }
    if (manifest->epoch == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::unexpected(
            collection_error(errc::io, "collection epoch space exhausted", store_.root_ / "gc"));
    }
    auto target_objects = std::set<std::string>{};
    auto target_chunks = std::set<std::string>{};
    for (const auto& id : ids) {
        const auto key = to_string(id);
        const auto object = store_.objects_.find(key);
        if (object == store_.objects_.end()) {
            continue;
        }
        target_objects.insert(key);
        for (const auto& chunk : object->second.chunks) {
            target_chunks.insert(to_string(chunk));
        }
        if (object->second.has_descriptor) {
            target_chunks.insert(to_string(object->second.descriptor));
        }
    }
    if (target_objects.empty()) {
        co_return std::unexpected(
            error{errc::not_found, "redaction target is not present", "store::redact"});
    }
    co_return co_await remove_local(std::move(*manifest),
                                    std::move(target_objects),
                                    std::move(target_chunks),
                                    std::nullopt,
                                    true,
                                    false);
}

runtime::task<void> collector::evict(digest id) {
    auto [lock_error, guard] = co_await store_.mutation_.scoped_lock();
    if (lock_error) {
        co_return std::unexpected(error{errc::busy, lock_error.message(), "store::evict"});
    }
    auto manifest = co_await load_collection_manifest(store_.root_);
    if (!manifest) {
        co_return std::unexpected(manifest.error());
    }
    if (manifest->epoch == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::unexpected(
            collection_error(errc::io, "collection epoch space exhausted", store_.root_ / "gc"));
    }
    const auto key = to_string(id);
    const auto remote = store_.remotes_.find(key);
    if (remote == store_.remotes_.end()) {
        co_return std::unexpected(error{errc::not_remote, "content is not registered remote", key});
    }

    auto target_objects = std::set<std::string>{};
    auto target_chunks = std::set<std::string>{};
    auto refresh_subjects = false;
    for (const auto& chunk : remote->second.chunks) {
        target_chunks.insert(to_string(chunk));
    }
    const auto object = store_.objects_.find(key);
    if (object != store_.objects_.end()) {
        target_objects.insert(key);
        refresh_subjects = object->second.kind == "attestation";
        for (const auto& chunk : object->second.chunks) {
            target_chunks.insert(to_string(chunk));
        }
        if (object->second.has_descriptor) {
            target_chunks.insert(to_string(object->second.descriptor));
        }
    }
    co_return co_await remove_local(std::move(*manifest),
                                    std::move(target_objects),
                                    std::move(target_chunks),
                                    key,
                                    false,
                                    refresh_subjects);
}

} // namespace liquid::store_detail
