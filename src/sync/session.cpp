// SPDX-License-Identifier: MIT

#include "liquid/sync.hpp"

#include <algorithm>
#include <array>
#include <deque>
#include <map>
#include <set>
#include <utility>

#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/store_access.hpp"
#include "store/references.hpp"

namespace liquid {
namespace {

auto sync_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto read_remote_object(remote_blob_source& peer, std::string instance, digest id)
    -> runtime::task<std::vector<std::byte>> {
    constexpr auto block_size = std::size_t{64U * 1024U};
    auto output = std::vector<std::byte>{};
    for (;;) {
        auto block = std::vector<std::byte>(block_size);
        auto read = co_await peer.read_range(instance, id, output.size(), block);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read > block.size()) {
            co_return std::unexpected(
                sync_error(errc::corrupt, "remote range exceeded its output span", to_string(id)));
        }
        output.insert(
            output.end(), block.begin(), block.begin() + static_cast<std::ptrdiff_t>(*read));
        if (*read < block.size()) {
            break;
        }
    }
    auto actual = hash(id.alg, output);
    if (!actual || !same_digest(*actual, id)) {
        co_return std::unexpected(
            sync_error(errc::corrupt, "remote object digest verification failed", to_string(id)));
    }
    co_return output;
}

} // namespace

struct sync_session::impl {
    store* local{};
    remote_blob_source* peer{};
    std::string source;
    std::string instance;
};

sync_session::sync_session(store& local, remote_blob_source& peer, std::string instance)
    : sync_session(local, peer, instance, instance) {}

sync_session::sync_session(store& local,
                           remote_blob_source& peer,
                           std::string source,
                           std::string instance)
    : impl_(std::make_unique<impl>(impl{&local, &peer, std::move(source), std::move(instance)})) {}

sync_session::~sync_session() = default;
sync_session::sync_session(sync_session&&) noexcept = default;
sync_session& sync_session::operator=(sync_session&&) noexcept = default;

runtime::task<sync_transfer_report> sync_session::send(std::span<const digest> roots) {
    if (!impl_ || !impl_->local || !impl_->peer) {
        co_return std::unexpected(sync_error(errc::invalid_argument, "sync session is empty"));
    }
    struct pending_object {
        digest id;
        bool payload{};
    };
    auto pending = std::deque<pending_object>{};
    for (const auto& root : roots) {
        pending.push_back({root, false});
    }
    auto visited = std::set<std::pair<algorithm, std::array<std::byte, 32>>>{};
    auto objects = std::vector<digest>{};
    auto contents = std::map<std::string, std::vector<std::byte>>{};
    auto registry = store_detail::reference_registry{};
    while (!pending.empty()) {
        const auto item = pending.front();
        pending.pop_front();
        if (!visited.emplace(item.id.alg, item.id.bytes).second) {
            continue;
        }
        objects.push_back(item.id);
        if (item.payload) {
            continue;
        }
        auto bytes = co_await impl_->local->get(item.id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        const auto kind = store_detail::detect_object_kind(*bytes);
        const auto references = registry.extract(kind, *bytes);
        if (kind == "blob") {
            auto blob = objects::decode_blob(*bytes);
            if (!blob) {
                co_return std::unexpected(blob.error());
            }
            for (const auto& chunk : blob->chunks) {
                pending.push_back({chunk, true});
            }
            for (const auto& entry : blob->format_entries) {
                pending.push_back({entry, false});
            }
        } else {
            const auto payload = kind == "chunk-list";
            for (const auto& reference : references.digests) {
                pending.push_back({reference, payload});
            }
        }
        contents.emplace(to_string(item.id), std::move(*bytes));
    }
    auto missing = co_await impl_->peer->find_missing(impl_->instance, objects);
    if (!missing) {
        co_return std::unexpected(missing.error());
    }
    // Validate the whole response before loading payloads or uploading any object.
    for (const auto& id : *missing) {
        if (!visited.contains({id.alg, id.bytes})) {
            co_return std::unexpected(
                sync_error(errc::corrupt,
                           "remote requested an object outside the offered closure",
                           to_string(id)));
        }
    }
    auto report = sync_transfer_report{};
    for (const auto& id : *missing) {
        auto found = contents.find(to_string(id));
        if (found == contents.end()) {
            auto bytes = co_await impl_->local->get(id);
            if (!bytes) {
                co_return std::unexpected(bytes.error());
            }
            found = contents.emplace(to_string(id), std::move(*bytes)).first;
        }
        report.bytes += found->second.size();
        auto written = co_await impl_->peer->write(impl_->instance, id, found->second);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        ++report.objects;
    }
    co_return report;
}

runtime::task<sync_transfer_report> sync_session::receive(std::span<const digest> roots) {
    if (!impl_ || !impl_->local || !impl_->peer) {
        co_return std::unexpected(sync_error(errc::invalid_argument, "sync session is empty"));
    }
    auto pending = std::deque<digest>{roots.begin(), roots.end()};
    auto visited = std::set<std::string>{};
    auto registry = store_detail::reference_registry{};
    auto report = sync_transfer_report{};
    while (!pending.empty()) {
        const auto id = pending.front();
        pending.pop_front();
        if (!visited.insert(to_string(id)).second) {
            continue;
        }
        auto present = co_await impl_->local->contains(id);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        auto bytes = std::vector<std::byte>{};
        if (*present) {
            auto loaded = co_await impl_->local->get(id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            bytes = std::move(*loaded);
        } else {
            auto loaded = co_await read_remote_object(*impl_->peer, impl_->instance, id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            report.bytes += loaded->size();
            auto stored =
                co_await repository_detail::store_access::put_verified(*impl_->local, id, *loaded);
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
            if (!same_digest(*stored, id)) {
                co_return std::unexpected(
                    sync_error(errc::corrupt, "stored object identity changed", to_string(id)));
            }
            ++report.objects;
            bytes = std::move(*loaded);
        }
        const auto kind = store_detail::detect_object_kind(bytes);
        const auto references = registry.extract(kind, bytes);
        for (const auto& reference : references.digests) {
            pending.push_back(reference);
        }
    }
    co_return report;
}

runtime::task<sync_metadata_report> sync_session::receive_metadata(std::span<const digest> roots) {
    if (!impl_ || !impl_->local || !impl_->peer) {
        co_return std::unexpected(sync_error(errc::invalid_argument, "sync session is empty"));
    }
    if (impl_->source.empty()) {
        co_return std::unexpected(
            sync_error(errc::invalid_argument, "metadata source id is empty"));
    }
    auto pending = std::deque<digest>{roots.begin(), roots.end()};
    auto visited = std::set<std::string>{};
    auto registry = store_detail::reference_registry{};
    auto report = sync_metadata_report{};
    while (!pending.empty()) {
        const auto id = pending.front();
        pending.pop_front();
        if (!visited.insert(to_string(id)).second) {
            continue;
        }
        auto present = co_await impl_->local->contains(id);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        auto bytes = std::vector<std::byte>{};
        if (*present) {
            auto loaded = co_await impl_->local->get(id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            bytes = std::move(*loaded);
        } else {
            auto loaded = co_await read_remote_object(*impl_->peer, impl_->instance, id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            auto stored =
                co_await repository_detail::store_access::put_verified(*impl_->local, id, *loaded);
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
            if (!same_digest(*stored, id)) {
                co_return std::unexpected(
                    sync_error(errc::corrupt, "stored metadata identity changed", to_string(id)));
            }
            bytes = std::move(*loaded);
        }
        const auto kind = store_detail::detect_object_kind(bytes);
        if (kind != "blob") {
            const auto references = registry.extract(kind, bytes);
            for (const auto& reference : references.digests) {
                pending.push_back(reference);
            }
            if (kind == "tree" && !*present) {
                ++report.trees;
            }
            continue;
        }
        auto blob = objects::decode_blob(bytes);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        const auto remote = remote_blob_location{impl_->source, impl_->instance};
        const auto whole_content =
            blob->content.alg == algorithm::blake3 || blob->content.alg == algorithm::sha256;
        if (whole_content) {
            blob->remote = remote;
            auto registered = co_await impl_->local->register_remote(*blob);
            if (!registered) {
                co_return std::unexpected(registered.error());
            }
        }
        for (auto index = std::size_t{}; index < blob->chunks.size(); ++index) {
            if (!whole_content) {
                auto chunk = liquid::blob{.chunks = {blob->chunks[index]},
                                          .size = blob->chunk_sizes[index],
                                          .chunk_sizes = {blob->chunk_sizes[index]},
                                          .content = blob->chunks[index],
                                          .remote = remote};
                auto registered = co_await impl_->local->register_remote(std::move(chunk));
                if (!registered) {
                    co_return std::unexpected(registered.error());
                }
            }
            auto chunk_present = co_await impl_->local->contains(blob->chunks[index]);
            if (!chunk_present) {
                co_return std::unexpected(chunk_present.error());
            }
            if (!*chunk_present) {
                report.bytes_remote += blob->chunk_sizes[index];
            }
        }
        for (const auto& entry : blob->format_entries) {
            pending.push_back(entry);
        }
        if (!*present) {
            ++report.blobs;
        }
    }
    co_return report;
}

} // namespace liquid
