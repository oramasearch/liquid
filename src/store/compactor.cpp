#include "compactor.hpp"

#include <algorithm>
#include <limits>
#include <string>
#include <system_error>
#include <utility>

#include "faults.hpp"
#include "pack.hpp"
#include "store_impl.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::uint64_t frame_header_size = 49;

std::filesystem::path pack_path(const std::filesystem::path& root, std::uint32_t id) {
    return root / "packs" / ("pack-" + std::to_string(id) + ".pack");
}

std::filesystem::path pending_path(const std::filesystem::path& root,
                                   std::uint64_t epoch,
                                   std::uint32_t old_pack_id,
                                   std::uint32_t new_pack_id) {
    return root / "gc" /
           ("compact-" + std::to_string(epoch) + "-" + std::to_string(old_pack_id) + "-" +
            std::to_string(new_pack_id) + ".pending");
}

error compaction_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}

void mark_all(std::vector<std::byte>& bitmap, std::size_t count) {
    bitmap.assign((count + 7) / 8, std::byte{0xff});
}

} // namespace

void compactor::describe(const collection_manifest& manifest, collection_report& report) const {
    for (const auto& [pack_id, pack] : store_.packs_) {
        std::uint64_t total_bytes = 0;
        std::uint64_t live_bytes = 0;
        std::uint64_t dead_bytes = 0;
        for (std::size_t frame = 0; frame != pack.entries.size(); ++frame) {
            const auto frame_bytes = frame_header_size + pack.entries[frame].location.length;
            total_bytes += frame_bytes;
            if (manifest.dead(pack_id, frame)) {
                dead_bytes += frame_bytes;
            } else {
                live_bytes += frame_bytes;
            }
        }
        if (total_bytes == 0 || dead_bytes == 0 || live_bytes == 0) {
            continue;
        }
        report.bytes_reclaimable_in_partially_dead_packs += dead_bytes;
        report.partially_dead_packs.push_back(
            {pack_id, static_cast<double>(live_bytes) / static_cast<double>(total_bytes)});
    }
}

runtime::task<void> compactor::run(collection_options options,
                                   collection_manifest manifest,
                                   collection_report& report) {
    if (options.compact_below == 0.0) {
        co_return result<void>{};
    }
    if (options.compact_only) {
        describe(manifest, report);
    }
    std::vector<std::uint32_t> candidates;
    for (const auto& partial : report.partially_dead_packs) {
        if (partial.live_fraction < options.compact_below) {
            candidates.push_back(partial.id);
        }
    }
    if (options.dry_run) {
        co_return result<void>{};
    }
    for (const auto old_pack_id : candidates) {
        auto compacted = co_await compact(old_pack_id, manifest, report);
        if (!compacted) {
            co_return std::unexpected(compacted.error());
        }
    }
    co_return result<void>{};
}

runtime::task<void> compactor::compact(std::uint32_t old_pack_id,
                                       collection_manifest& manifest,
                                       collection_report& report) {
    const auto old_it = store_.packs_.find(old_pack_id);
    if (old_it == store_.packs_.end()) {
        co_return result<void>{};
    }
    auto& old_pack = old_it->second;
    std::uint32_t new_pack_id = store_.index_.highest_pack_id();
    for (const auto& [pack_id, ignored] : store_.packs_) {
        new_pack_id = std::max(new_pack_id, pack_id);
    }
    if (new_pack_id == std::numeric_limits<std::uint32_t>::max()) {
        co_return std::unexpected(
            compaction_error(errc::io, "pack id space exhausted", store_.root_ / "packs"));
    }
    ++new_pack_id;
    if (old_pack_id == store_.current_pack_ &&
        new_pack_id == std::numeric_limits<std::uint32_t>::max()) {
        co_return std::unexpected(
            compaction_error(errc::io, "pack id space exhausted", store_.root_ / "packs"));
    }
    if (manifest.epoch == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::unexpected(
            compaction_error(errc::io, "collection epoch space exhausted", store_.root_ / "gc"));
    }

    if (old_pack_id == store_.current_pack_ && old_pack.appendable) {
        auto sealed = co_await seal_pack(
            pack_path(store_.root_, old_pack_id), old_pack, store_.options_.durable);
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        old_pack.appendable = false;
    }

    const auto next_epoch = manifest.epoch + 1;
    const auto marker = pending_path(store_.root_, next_epoch, old_pack_id, new_pack_id);
    auto marker_written = co_await write_file(marker, {});
    if (!marker_written) {
        co_return std::unexpected(marker_written.error());
    }
    pack_contents replacement{new_pack_id, {}, 16};
    std::vector<index_entry> moved;
    for (std::size_t frame = 0; frame != old_pack.entries.size(); ++frame) {
        if (manifest.dead(old_pack_id, frame)) {
            continue;
        }
        auto copied = co_await append_pack_frame(pack_path(store_.root_, new_pack_id),
                                                 replacement,
                                                 pack_path(store_.root_, old_pack_id),
                                                 old_pack.entries[frame],
                                                 store_.options_.durable);
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
        replacement.entries.push_back(*copied);
        replacement.data_end += frame_header_size + copied->location.length;
        moved.push_back({copied->id, copied->location});
    }
    auto sealed = co_await seal_pack(
        pack_path(store_.root_, new_pack_id), replacement, store_.options_.durable);
    if (!sealed) {
        co_return std::unexpected(sealed.error());
    }
    replacement.appendable = false;

    collection_manifest committed = manifest;
    committed.epoch = next_epoch;
    mark_all(committed.tombstones[old_pack_id], old_pack.entries.size());
    auto saved = co_await save_collection_manifest(store_.root_, committed);
    if (!saved) {
        co_return std::unexpected(saved.error());
    }
    for (const auto& entry : moved) {
        auto inserted = store_.index_.insert(entry);
        if (!inserted) {
            co_return std::unexpected(inserted.error());
        }
    }
    store_.index_.set_highest_pack_id(new_pack_id);
    store_.index_.set_collection_epoch(committed.epoch);
    auto indexed = co_await store_.index_.flush();
    if (!indexed) {
        co_return std::unexpected(indexed.error());
    }

    const auto old_path = pack_path(store_.root_, old_pack_id);
    std::error_code old_size_error;
    const auto old_bytes = std::filesystem::file_size(old_path, old_size_error);
    try {
        visit_fault({fault_operation::remove, fault_phase::before, old_path});
        std::filesystem::remove(old_path);
        visit_fault({fault_operation::remove, fault_phase::after, old_path});
        std::error_code ignored;
        std::filesystem::remove(marker, ignored);
    } catch (const std::system_error& caught) {
        co_return std::unexpected(compaction_error(errc::io, caught.what(), old_path));
    }
    store_.packs_.erase(old_pack_id);
    store_.packs_.emplace(new_pack_id, std::move(replacement));
    if (old_pack_id == store_.current_pack_) {
        store_.current_pack_ = new_pack_id + 1;
        store_.packs_.emplace(store_.current_pack_, pack_contents{store_.current_pack_, {}, 16});
    }
    std::error_code size_error;
    const auto replacement_bytes =
        std::filesystem::file_size(pack_path(store_.root_, new_pack_id), size_error);
    if (!size_error) {
        report.bytes_written_by_compaction += replacement_bytes;
        if (!old_size_error && old_bytes > replacement_bytes) {
            report.bytes_reclaimed_by_compaction += old_bytes - replacement_bytes;
        }
    }
    ++report.packs_compacted;
    report.epoch = committed.epoch;
    manifest = std::move(committed);
    co_return result<void>{};
}

} // namespace liquid::store_detail
