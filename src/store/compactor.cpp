// SPDX-License-Identifier: MIT

#include "compactor.hpp"

#include <algorithm>
#include <limits>
#include <set>
#include <string>
#include <system_error>
#include <utility>

#include "backend.hpp"
#include "faults.hpp"
#include "pack.hpp"
#include "store_impl.hpp"

namespace liquid::store_detail {
namespace {

constexpr std::uint64_t frame_header_size = 49;

std::string pack_key(std::uint32_t id) {
    return "packs/pack-" + std::to_string(id) + ".pack";
}

std::string pending_key(std::uint64_t epoch, std::uint32_t old_pack_id, std::uint32_t new_pack_id) {
    return "gc/compact-" + std::to_string(epoch) + "-" + std::to_string(old_pack_id) + "-" +
           std::to_string(new_pack_id) + ".pending";
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
    if (options.compact_below == 0.0 && !options.repack) {
        co_return result<void>{};
    }
    if (options.compact_only) {
        describe(manifest, report);
    }
    std::set<std::uint32_t> candidates;
    for (const auto& partial : report.partially_dead_packs) {
        if (partial.live_fraction < options.compact_below) {
            candidates.insert(partial.id);
        }
    }
    if (options.repack) {
        for (const auto& [pack_id, pack] : store_.packs_) {
            if (std::ranges::any_of(
                    pack.entries, [](const pack_entry& entry) { return entry.base.has_value(); })) {
                candidates.insert(pack_id);
            }
        }
    }
    if (options.dry_run) {
        co_return result<void>{};
    }
    for (const auto old_pack_id : candidates) {
        auto compacted = co_await compact(old_pack_id, manifest, report, options.repack);
        if (!compacted) {
            co_return std::unexpected(compacted.error());
        }
    }
    co_return result<void>{};
}

runtime::task<void> compactor::compact(std::uint32_t old_pack_id,
                                       collection_manifest& manifest,
                                       collection_report& report,
                                       bool rebase_deltas) {
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
        auto sealed = co_await seal_pack(store_.persistence_, pack_key(old_pack_id), old_pack);
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        old_pack.appendable = false;
    }

    const auto next_epoch = manifest.epoch + 1;
    const auto marker = pending_key(next_epoch, old_pack_id, new_pack_id);
    auto marker_written = co_await store_.persistence_->put(marker, {});
    if (!marker_written) {
        co_return std::unexpected(marker_written.error());
    }
    // The marker is what lets recovery remove an uncommitted replacement, so it must be durable
    // before any byte of the replacement (liquid #344): a backend may keep any subset of the
    // writes issued between two barriers, and a replacement kept without its marker would be an
    // orphan pack no recovery or collection accounts for.
    auto marker_ordered = co_await store_.persistence_->barrier();
    if (!marker_ordered) {
        co_return std::unexpected(marker_ordered.error());
    }
    pack_contents replacement{new_pack_id, {}, 16};
    std::vector<index_entry> moved;
    for (std::size_t frame = 0; frame != old_pack.entries.size(); ++frame) {
        if (manifest.dead(old_pack_id, frame)) {
            continue;
        }
        auto copied =
            result<pack_entry>{std::unexpected(error{errc::internal,
                                                     "pack rewrite was not selected",
                                                     to_string(old_pack.entries[frame].id)})};
        if (rebase_deltas && old_pack.entries[frame].base) {
            auto raw = co_await store_.load_chunk(old_pack.entries[frame].id);
            if (!raw) {
                co_return std::unexpected(raw.error());
            }
            auto base = *old_pack.entries[frame].base;
            auto visited = std::set<std::string>{};
            while (true) {
                const auto parent = store_.chunk_bases_.find(to_string(base));
                if (parent == store_.chunk_bases_.end()) {
                    break;
                }
                if (!visited.insert(to_string(base)).second) {
                    co_return std::unexpected(compaction_error(
                        errc::corrupt, "delta chunk reference cycle", pack_key(old_pack_id)));
                }
                base = parent->second;
            }
            auto dictionary = co_await store_.load_chunk(base);
            if (!dictionary) {
                co_return std::unexpected(dictionary.error());
            }
            copied = co_await append_pack_entry(store_.persistence_,
                                                pack_key(new_pack_id),
                                                replacement,
                                                old_pack.entries[frame].id,
                                                std::move(*raw),
                                                std::nullopt,
                                                store_.options_.durable,
                                                {{base, std::move(*dictionary)}});
            ++report.deltas_rebased;
        } else {
            copied = co_await append_pack_frame(store_.persistence_,
                                                pack_key(new_pack_id),
                                                replacement,
                                                pack_key(old_pack_id),
                                                old_pack.entries[frame],
                                                store_.options_.durable);
        }
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
        replacement.entries.push_back(*copied);
        replacement.data_end += frame_header_size + copied->location.length;
        moved.push_back({copied->id, copied->location});
    }
    auto sealed = co_await seal_pack(store_.persistence_, pack_key(new_pack_id), replacement);
    if (!sealed) {
        co_return std::unexpected(sealed.error());
    }
    replacement.appendable = false;

    collection_manifest committed = manifest;
    committed.epoch = next_epoch;
    mark_all(committed.tombstones[old_pack_id], old_pack.entries.size());
    auto saved = co_await save_collection_manifest(store_.persistence_, committed);
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

    auto old_bytes = co_await backend_size(store_.persistence_, pack_key(old_pack_id));
    auto removed = co_await store_.persistence_->remove(pack_key(old_pack_id));
    if (!removed && removed.error().code != errc::not_found) {
        co_return std::unexpected(removed.error());
    }
    // The old pack's removal must be durable before the marker's, for the same reason: without the
    // marker, recovery no longer knows the old pack is to go.
    auto removal_ordered = co_await store_.persistence_->barrier();
    if (!removal_ordered) {
        co_return std::unexpected(removal_ordered.error());
    }
    removed = co_await store_.persistence_->remove(marker);
    if (!removed && removed.error().code != errc::not_found) {
        co_return std::unexpected(removed.error());
    }
    store_.packs_.erase(old_pack_id);
    store_.packs_.emplace(new_pack_id, std::move(replacement));
    for (const auto& entry : store_.packs_.at(new_pack_id).entries) {
        if (entry.base) {
            store_.chunk_bases_.insert_or_assign(to_string(entry.id), *entry.base);
        } else {
            store_.chunk_bases_.erase(to_string(entry.id));
        }
    }
    if (old_pack_id == store_.current_pack_) {
        store_.current_pack_ = new_pack_id + 1;
        store_.packs_.emplace(store_.current_pack_, pack_contents{store_.current_pack_, {}, 16});
    }
    auto replacement_bytes = co_await backend_size(store_.persistence_, pack_key(new_pack_id));
    if (replacement_bytes) {
        report.bytes_written_by_compaction += *replacement_bytes;
        if (old_bytes && *old_bytes > *replacement_bytes) {
            report.bytes_reclaimed_by_compaction += *old_bytes - *replacement_bytes;
        }
    }
    ++report.packs_compacted;
    report.epoch = committed.epoch;
    manifest = std::move(committed);
    co_return result<void>{};
}

} // namespace liquid::store_detail
