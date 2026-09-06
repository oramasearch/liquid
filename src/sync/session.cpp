#include "liquid/sync.hpp"

#include <algorithm>
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
    auto pending = std::deque<digest>{roots.begin(), roots.end()};
    auto visited = std::set<std::string>{};
    auto objects = std::vector<digest>{};
    auto contents = std::map<std::string, std::vector<std::byte>>{};
    auto registry = store_detail::reference_registry{};
    while (!pending.empty()) {
        const auto id = pending.front();
        pending.pop_front();
        if (!visited.insert(to_string(id)).second) {
            continue;
        }
        auto bytes = co_await impl_->local->get(id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        const auto kind = store_detail::detect_object_kind(*bytes);
        const auto references = registry.extract(kind, *bytes);
        for (const auto& reference : references.digests) {
            pending.push_back(reference);
        }
        objects.push_back(id);
        contents.emplace(to_string(id), std::move(*bytes));
    }
    auto missing = co_await impl_->peer->find_missing(impl_->instance, objects);
    if (!missing) {
        co_return std::unexpected(missing.error());
    }
    auto report = sync_transfer_report{};
    for (const auto& id : *missing) {
        const auto found = contents.find(to_string(id));
        if (found == contents.end()) {
            co_return std::unexpected(
                sync_error(errc::corrupt, "peer requested an unknown object", to_string(id)));
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
    enum class metadata_kind : std::uint8_t { tree, blob };
    struct pending_metadata {
        digest id;
        metadata_kind kind{metadata_kind::tree};
    };
    auto pending = std::deque<pending_metadata>{};
    for (const auto& root : roots) {
        pending.push_back({root, metadata_kind::tree});
    }
    auto visited = std::set<std::string>{};
    auto report = sync_metadata_report{};
    while (!pending.empty()) {
        const auto item = pending.front();
        pending.pop_front();
        if (!visited.insert(to_string(item.id)).second) {
            continue;
        }
        auto present = co_await impl_->local->contains(item.id);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        auto bytes = std::vector<std::byte>{};
        if (*present) {
            auto loaded = co_await impl_->local->get(item.id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            bytes = std::move(*loaded);
        } else {
            auto loaded = co_await read_remote_object(*impl_->peer, impl_->instance, item.id);
            if (!loaded) {
                co_return std::unexpected(loaded.error());
            }
            auto stored = co_await repository_detail::store_access::put_verified(
                *impl_->local, item.id, *loaded);
            if (!stored) {
                co_return std::unexpected(stored.error());
            }
            if (!same_digest(*stored, item.id)) {
                co_return std::unexpected(sync_error(
                    errc::corrupt, "stored metadata identity changed", to_string(item.id)));
            }
            bytes = std::move(*loaded);
            if (item.kind == metadata_kind::tree) {
                ++report.trees;
            } else {
                ++report.blobs;
            }
        }
        if (item.kind == metadata_kind::tree) {
            auto tree = objects::decode_tree(bytes);
            if (!tree) {
                co_return std::unexpected(tree.error());
            }
            for (const auto& entry : tree->entries) {
                if (entry.kind == node_kind::tree || entry.kind == node_kind::repo) {
                    pending.push_back({entry.ref, metadata_kind::tree});
                    continue;
                }
                if (entry.kind == node_kind::blob) {
                    pending.push_back({entry.ref, metadata_kind::blob});
                    continue;
                }
                co_return std::unexpected(
                    sync_error(errc::corrupt, "image tree contains a non-file entry", entry.name));
            }
            continue;
        }
        auto blob = objects::decode_blob(bytes);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        if (blob->content.alg != algorithm::blake3 && blob->content.alg != algorithm::sha256) {
            co_return std::unexpected(sync_error(errc::unsupported,
                                                 "blob record has no remote-capable content digest",
                                                 to_string(item.id)));
        }
        blob->remote = remote_blob_location{impl_->source, impl_->instance};
        auto registered = co_await impl_->local->register_remote(*blob);
        if (!registered) {
            co_return std::unexpected(registered.error());
        }
        for (auto index = std::size_t{}; index < blob->chunks.size(); ++index) {
            auto chunk_present = co_await impl_->local->contains(blob->chunks[index]);
            if (!chunk_present) {
                co_return std::unexpected(chunk_present.error());
            }
            if (!*chunk_present) {
                report.bytes_remote += blob->chunk_sizes[index];
            }
        }
    }
    co_return report;
}

} // namespace liquid
