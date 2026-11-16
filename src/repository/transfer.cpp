// SPDX-License-Identifier: MIT

// Whole-repository transfer between two open stores (liquid #328). See
// docs/library/store.md#whole-repository-transfer.

#include "repository/transfer.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/store_access.hpp"
#include "store/backend.hpp"
#include "store/leases.hpp"
#include "store/references.hpp"
#include "store/store_impl.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;

constexpr auto journal_pointer_prefix = std::string_view{"liquid-journal-v2 "};
// The structural-merge parse graph is a cache beside the state, rebuilt on demand; it is not
// repository state and is neither copied nor compared.
constexpr auto merge_graph_name = std::string_view{"STRUCTURAL_MERGE_GRAPH"};
// Nested CBOR byte strings (signed drip bodies) are searched for references to this depth.
constexpr auto maximum_record_depth = std::size_t{8};
// An object stored without a recognised kind is searched for references only up to this size.
constexpr auto maximum_unknown_record = std::uint64_t{1024U * 1024U};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto text_of(std::span<const std::byte> bytes) -> std::string_view {
    return {reinterpret_cast<const char*>(bytes.data()), bytes.size()};
}

auto same_bytes(const std::optional<std::vector<std::byte>>& left,
                const std::vector<std::byte>& right) -> bool {
    return left && *left == right;
}

// A digest as a fixed-size set key: the algorithm byte, then the 32 digest bytes.
struct identity_key {
    std::array<std::byte, 33> bytes{};

    auto operator==(const identity_key&) const -> bool = default;
};

struct identity_hash {
    auto operator()(const identity_key& key) const noexcept -> std::size_t {
        auto value = std::size_t{};
        std::memcpy(&value, key.bytes.data() + 1, sizeof value);
        return value ^ std::to_integer<std::size_t>(key.bytes[0]);
    }
};

auto key_of(const digest& id) -> identity_key {
    auto key = identity_key{};
    key.bytes[0] = std::byte{static_cast<unsigned char>(id.alg)};
    std::ranges::copy(id.bytes, key.bytes.begin() + 1);
    return key;
}

auto valid_identity(const digest& id) -> bool {
    const auto known = id.alg == algorithm::blake3 || id.alg == algorithm::sha256 ||
                       id.alg == algorithm::blake3_keyed;
    return known &&
           std::ranges::any_of(id.bytes, [](std::byte value) { return value != std::byte{}; });
}

void collect_candidates(const cbor::value& value, std::vector<digest>& output, std::size_t depth);

// A byte string may be a digest's text (repository metadata stores some references that way) or
// a nested CBOR record (a signed drip body).
void collect_candidates(std::span<const std::byte> bytes,
                        std::vector<digest>& output,
                        std::size_t depth) {
    if (bytes.empty()) {
        return;
    }
    if (bytes.size() <= 128) {
        if (auto parsed = parse_digest(text_of(bytes))) {
            output.push_back(*parsed);
            return;
        }
    }
    const auto major = std::to_integer<unsigned>(bytes.front()) >> 5U;
    if (depth >= maximum_record_depth || (major != 4U && major != 5U)) {
        return;
    }
    if (auto decoded = cbor::decode(bytes)) {
        collect_candidates(*decoded, output, depth + 1);
    }
}

// Every digest-shaped value in a decoded record: a map {alg, bytes} as the repository document
// encodes a digest, a digest's text, and the same inside nested records. A candidate is only a
// reference if the source store holds it, so a value that merely looks like a digest costs one
// lookup and copies nothing.
void collect_candidates(const cbor::value& value, std::vector<digest>& output, std::size_t depth) {
    if (const auto* text = std::get_if<std::string>(&value.data)) {
        if (text->size() <= 128) {
            if (auto parsed = parse_digest(*text)) {
                output.push_back(*parsed);
            }
        }
        return;
    }
    if (const auto* bytes = std::get_if<cbor::value::bytes>(&value.data)) {
        collect_candidates(std::span<const std::byte>{*bytes}, output, depth);
        return;
    }
    if (const auto* array = std::get_if<cbor::value::array>(&value.data)) {
        for (const auto& item : *array) {
            collect_candidates(item, output, depth);
        }
        return;
    }
    const auto* map = std::get_if<cbor::value::map>(&value.data);
    if (!map) {
        return;
    }
    const cbor::value* alg = nullptr;
    const cbor::value* bytes = nullptr;
    for (const auto& [name, item] : *map) {
        if (const auto* key = std::get_if<std::string>(&name.data)) {
            if (*key == "alg") {
                alg = &item;
            } else if (*key == "bytes") {
                bytes = &item;
            }
        }
        collect_candidates(item, output, depth);
    }
    const auto* algorithm_value = alg ? std::get_if<std::uint64_t>(&alg->data) : nullptr;
    const auto* digest_bytes = bytes ? std::get_if<cbor::value::bytes>(&bytes->data) : nullptr;
    if (algorithm_value && digest_bytes && *algorithm_value <= 3 && digest_bytes->size() == 32) {
        auto id = digest{static_cast<algorithm>(*algorithm_value), {}};
        std::ranges::copy(*digest_bytes, id.bytes.begin());
        output.push_back(id);
    }
}

auto leaf_name(std::string_view key) -> std::string_view {
    const auto separator = key.rfind('/');
    return separator == std::string_view::npos ? key : key.substr(separator + 1);
}

auto is_cache(std::string_view key) -> bool {
    auto rest = key;
    while (!rest.empty()) {
        const auto separator = rest.find('/');
        const auto part = rest.substr(0, separator);
        if (part.starts_with(merge_graph_name) || part.ends_with(".tmp")) {
            return true;
        }
        if (separator == std::string_view::npos) {
            break;
        }
        rest.remove_prefix(separator + 1);
    }
    return false;
}

// The repository's state keys, relative to its prefix, with their bytes. State objects are small
// pointers and cursors; the documents they name are objects.
auto read_state(std::shared_ptr<store_backend> backend, std::string prefix)
    -> runtime::task<std::map<std::string, std::vector<std::byte>>> {
    auto listed = co_await backend->list(prefix);
    if (!listed) {
        co_return std::unexpected(listed.error());
    }
    auto output = std::map<std::string, std::vector<std::byte>>{};
    for (const auto& entry : *listed) {
        if (!entry.key.starts_with(prefix)) {
            continue;
        }
        auto name = entry.key.substr(prefix.size());
        if (name.empty() || is_cache(name)) {
            continue;
        }
        auto bytes = co_await store_detail::read_backend_object(backend, entry.key);
        if (!bytes) {
            if (bytes.error().code == errc::not_found) {
                continue;
            }
            co_return std::unexpected(bytes.error());
        }
        output.emplace(std::move(name), std::move(*bytes));
    }
    co_return output;
}

auto read_optional(std::shared_ptr<store_backend> backend, std::string key)
    -> runtime::task<std::optional<std::vector<std::byte>>> {
    auto bytes = co_await store_detail::read_backend_object(backend, std::move(key));
    if (!bytes) {
        if (bytes.error().code == errc::not_found) {
            co_return std::optional<std::vector<std::byte>>{};
        }
        co_return std::unexpected(bytes.error());
    }
    co_return std::optional<std::vector<std::byte>>{std::move(*bytes)};
}

auto parse_marker(std::span<const std::byte> bytes, std::string_view prefix, std::string context)
    -> result<digest> {
    auto text = text_of(bytes);
    text.remove_prefix(std::min(prefix.size(), text.size()));
    if (const auto newline = text.find('\n'); newline != std::string_view::npos) {
        text = text.substr(0, newline);
    }
    auto parsed = parse_digest(text);
    if (!parsed) {
        return std::unexpected(failure(
            errc::corrupt, "repository state does not name a document", std::move(context)));
    }
    return *parsed;
}

// The lease owner prefix repository handles write: `repository:<hex repository id>\n`.
auto lease_owner_prefix(std::span<const std::byte> repository_id) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{"repository:"};
    for (const auto value : repository_id) {
        const auto byte = std::to_integer<unsigned char>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    output.push_back('\n');
    return output;
}

struct reference {
    digest id{};
    // A strong reference must be present in the source; a weak one is copied only if it is.
    bool strong{};
};

void add_counts(repository_transfer_report& report, const store_detail::transfer_counts& counts) {
    report.objects_read += counts.objects_read;
    report.bytes_read += counts.bytes_read;
    report.objects_written += counts.objects_written;
    report.bytes_written += counts.bytes_written;
}

// A depth-first walk of everything reachable from the roots. An object is copied only after
// everything it references, so an object present in the destination already has everything it
// reaches and a rerun skips it whole. Content passes one chunk at a time; the walk keeps one
// fixed-size key per identity visited and the references of the objects on its current path.
class transfer_walk {
public:
    transfer_walk(store_detail::store_backend& source,
                  store_detail::store_backend& destination,
                  const repository_transfer_options& options,
                  repository_transfer_report& report)
        : source_(source), destination_(destination), options_(options), report_(report) {}

    auto run(const std::vector<reference>& roots) -> runtime::task<void> {
        for (const auto& root : roots) {
            auto visited = co_await visit(root);
            if (!visited) {
                co_return std::unexpected(visited.error());
            }
            while (!stack_.empty()) {
                auto& top = stack_.back();
                if (top.next < top.references.size()) {
                    const auto next = top.references[top.next++];
                    auto child = co_await visit(next);
                    if (!child) {
                        co_return std::unexpected(child.error());
                    }
                    continue;
                }
                auto done = std::move(stack_.back());
                stack_.pop_back();
                auto finished = co_await finish(done);
                if (!finished) {
                    co_return std::unexpected(finished.error());
                }
            }
        }
        co_return result<void>{};
    }

private:
    struct frame {
        store_detail::stored_object object;
        std::vector<reference> references;
        std::size_t next{};
    };

    [[nodiscard]] auto copying() const -> bool {
        return options_.mode == repository_transfer_mode::copy;
    }

    void differ(const digest& id, std::vector<std::string> reasons) {
        for (auto& reason : reasons) {
            report_.differences.push_back({to_string(id), std::move(reason)});
        }
    }

    auto visit(reference item) -> runtime::task<void> {
        if (!valid_identity(item.id) || !visited_.insert(key_of(item.id)).second) {
            co_return result<void>{};
        }
        auto held = source_.transfer_describe(item.id);
        if (!held) {
            co_return std::unexpected(held.error());
        }
        if (!held->object && !held->chunk && !held->remote) {
            if (item.strong) {
                report_.missing.push_back(item.id);
            }
            co_return result<void>{};
        }
        ++report_.objects;
        if (copying()) {
            auto there = destination_.transfer_describe(item.id);
            if (!there) {
                co_return std::unexpected(there.error());
            }
            const auto present = held->object  ? there->object
                                 : held->chunk ? there->chunk
                                               : there->remote || there->object;
            if (present) {
                ++report_.objects_present;
                co_return result<void>{};
            }
        }
        if (held->object) {
            auto references = co_await references_of(held->value);
            if (!references) {
                co_return std::unexpected(references.error());
            }
            stack_.push_back({std::move(held->value), std::move(*references), 0});
            co_return result<void>{};
        }
        if (held->chunk) {
            if (copying()) {
                auto copied = co_await destination_.transfer_copy_chunk(source_, item.id);
                if (!copied) {
                    co_return std::unexpected(copied.error());
                }
                add_counts(report_, *copied);
                co_return result<void>{};
            }
            auto verified = co_await destination_.transfer_verify_chunk(item.id);
            if (!verified) {
                co_return std::unexpected(verified.error());
            }
            differ(item.id, std::move(*verified));
            co_return result<void>{};
        }
        // A remote registration: the content lives elsewhere and the destination records the same
        // location. Whatever chunks the source holds locally are copied too.
        if (copying()) {
            auto registered = co_await destination_.transfer_copy_remote(held->registration);
            if (!registered) {
                co_return std::unexpected(registered.error());
            }
            add_counts(report_, *registered);
        } else {
            differ(item.id, destination_.transfer_verify_remote(held->registration));
        }
        for (const auto& chunk : held->registration.chunks) {
            auto visited = co_await visit({chunk, false});
            if (!visited) {
                co_return std::unexpected(visited.error());
            }
        }
        co_return result<void>{};
    }

    auto references_of(const store_detail::stored_object& object)
        -> runtime::task<std::vector<reference>> {
        auto output = std::vector<reference>{};
        for (const auto& attestation : source_.attestations(object.id)) {
            output.push_back({attestation, false});
        }
        const auto known = object.kind != "unknown";
        if (!known && object.size > maximum_unknown_record) {
            co_return output;
        }
        auto bytes = co_await source_.get(object.id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        // A record whose kind the store did not record at write time is searched as well, but its
        // references are weak: its bytes only resemble a record.
        const auto kind = known ? object.kind : store_detail::detect_object_kind(*bytes);
        const auto extracted = registry_.extract(kind, *bytes);
        for (const auto& id : extracted.digests) {
            output.push_back({id, known});
        }
        if (kind == "blob") {
            if (auto value = objects::decode_blob(*bytes)) {
                // The whole-content object of a blob record: present when the content was stored
                // whole (put_file, write), absent when only its chunks were.
                output.push_back({value->content, false});
            }
        }
        co_return output;
    }

    auto finish(const frame& done) -> runtime::task<void> {
        if (!copying()) {
            auto verified = co_await destination_.transfer_verify_object(done.object);
            if (!verified) {
                co_return std::unexpected(verified.error());
            }
            differ(done.object.id, std::move(*verified));
            co_return result<void>{};
        }
        auto copied = co_await destination_.transfer_copy_object(source_, done.object);
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
        add_counts(report_, *copied);
        if (++batched_ >= options_.batch_objects) {
            batched_ = 0;
            auto published = co_await destination_.transfer_publish();
            if (!published) {
                co_return std::unexpected(published.error());
            }
        }
        co_return result<void>{};
    }

    store_detail::store_backend& source_;
    store_detail::store_backend& destination_;
    const repository_transfer_options& options_;
    repository_transfer_report& report_;
    store_detail::reference_registry registry_;
    std::unordered_set<identity_key, identity_hash> visited_;
    std::vector<frame> stack_;
    std::uint64_t batched_{};
};

// The roots of a repository's reachable set and the lease records that retain it in the source.
struct transfer_plan {
    std::map<std::string, std::vector<std::byte>> state;
    std::vector<reference> roots;
    std::vector<store_detail::lease_record> leases;
};

auto plan_transfer(store_detail::store_backend& source,
                   std::shared_ptr<store_backend> backend,
                   const std::string& prefix) -> runtime::task<transfer_plan> {
    auto plan = transfer_plan{};
    auto state = co_await read_state(backend, prefix);
    if (!state) {
        co_return std::unexpected(state.error());
    }
    plan.state = std::move(*state);
    if (!plan.state.contains("LIQUID_REPO")) {
        co_return std::unexpected(failure(errc::not_found,
                                          "repository does not exist in the source store",
                                          prefix + "LIQUID_REPO"));
    }
    // Documents first: the checkpoint and journal of the repository and of each session.
    auto documents = std::vector<digest>{};
    auto candidates = std::vector<digest>{};
    for (const auto& [name, bytes] : plan.state) {
        const auto leaf = leaf_name(name);
        if (leaf == "LIQUID_REPO" ||
            (leaf == "LIQUID_JOURNAL" && text_of(bytes).starts_with(journal_pointer_prefix))) {
            const auto marker_prefix =
                leaf == "LIQUID_REPO" ? std::string_view{} : journal_pointer_prefix;
            auto document = parse_marker(bytes, marker_prefix, prefix + name);
            if (!document) {
                co_return std::unexpected(document.error());
            }
            documents.push_back(*document);
            plan.roots.push_back({*document, true});
            continue;
        }
        // A legacy journal holds its document inline; a session's authority and the cursors are
        // small records of their own.
        collect_candidates(std::span<const std::byte>{bytes}, candidates, 0);
    }
    auto repository_id = std::optional<cbor::value::bytes>{};
    for (const auto& document : documents) {
        auto held = source.transfer_describe(document);
        if (!held) {
            co_return std::unexpected(held.error());
        }
        if (!held->object && !held->chunk && !held->remote) {
            continue;
        }
        auto bytes = co_await source.get(document);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto decoded = cbor::decode(*bytes);
        if (!decoded) {
            co_return std::unexpected(failure(
                errc::corrupt, "repository document cannot be decoded", to_string(document)));
        }
        collect_candidates(*decoded, candidates, 0);
        if (repository_id) {
            continue;
        }
        if (const auto* fields = std::get_if<cbor::value::map>(&decoded->data)) {
            for (const auto& [name, value] : *fields) {
                const auto* key = std::get_if<std::string>(&name.data);
                const auto* id = std::get_if<cbor::value::bytes>(&value.data);
                if (key && *key == "repo" && id && id->size() == 16) {
                    repository_id = *id;
                }
            }
        }
    }
    for (const auto& candidate : candidates) {
        plan.roots.push_back({candidate, false});
    }
    if (repository_id) {
        const auto owner = lease_owner_prefix(*repository_id);
        auto leases = co_await store_detail::load_leases(backend, source.root_path() / "leases");
        if (!leases) {
            co_return std::unexpected(leases.error());
        }
        for (auto& record : *leases) {
            if (!record.owner.starts_with(owner)) {
                continue;
            }
            plan.roots.push_back({record.root, false});
            plan.leases.push_back(std::move(record));
        }
    }
    co_return plan;
}

// Writes one state key unless the destination already holds the same bytes.
auto write_state(std::shared_ptr<store_backend> into,
                 std::string key,
                 const std::vector<std::byte>& bytes,
                 const std::map<std::string, std::vector<std::byte>>& existing,
                 const std::string& name,
                 repository_transfer_report& report) -> runtime::task<void> {
    const auto found = existing.find(name);
    if (found != existing.end() && found->second == bytes) {
        co_return result<void>{};
    }
    auto written = co_await into->put(std::move(key), bytes);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    ++report.state_written;
    co_return result<void>{};
}

// Leases, then every state key but the repository's own LIQUID_REPO, then LIQUID_REPO: a reader
// of the destination finds either no repository or one whose every object is already present.
auto publish_state(const transfer_plan& plan,
                   std::shared_ptr<store_backend> from,
                   std::shared_ptr<store_backend> into,
                   const std::string& prefix,
                   repository_transfer_report& report) -> runtime::task<void> {
    for (const auto& record : plan.leases) {
        auto bytes = co_await store_detail::read_backend_object(from, record.key);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto existing = co_await read_optional(into, record.key);
        if (!existing) {
            co_return std::unexpected(existing.error());
        }
        if (same_bytes(*existing, *bytes)) {
            continue;
        }
        auto written = co_await into->put(record.key, std::move(*bytes));
        if (!written) {
            co_return std::unexpected(written.error());
        }
        ++report.leases_written;
    }
    auto ordered = co_await into->barrier();
    if (!ordered) {
        co_return std::unexpected(ordered.error());
    }
    auto existing = co_await read_state(into, prefix);
    if (!existing) {
        co_return std::unexpected(existing.error());
    }
    // State the source no longer has (a journal it checkpointed, a session it published) would
    // otherwise be read over the copied checkpoint.
    for (const auto& [name, bytes] : *existing) {
        static_cast<void>(bytes);
        if (plan.state.contains(name)) {
            continue;
        }
        auto removed = co_await into->remove(prefix + name);
        if (!removed && removed.error().code != errc::not_found) {
            co_return std::unexpected(removed.error());
        }
        ++report.state_written;
    }
    for (const auto& [name, bytes] : plan.state) {
        if (name == "LIQUID_REPO") {
            continue;
        }
        auto written = co_await write_state(into, prefix + name, bytes, *existing, name, report);
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    ordered = co_await into->barrier();
    if (!ordered) {
        co_return std::unexpected(ordered.error());
    }
    auto marker = co_await write_state(into,
                                       prefix + "LIQUID_REPO",
                                       plan.state.at("LIQUID_REPO"),
                                       *existing,
                                       "LIQUID_REPO",
                                       report);
    if (!marker) {
        co_return std::unexpected(marker.error());
    }
    co_return co_await into->barrier();
}

auto compare_state(const transfer_plan& plan,
                   std::shared_ptr<store_backend> from,
                   std::shared_ptr<store_backend> into,
                   const std::string& prefix,
                   repository_transfer_report& report) -> runtime::task<void> {
    for (const auto& record : plan.leases) {
        auto expected = co_await store_detail::read_backend_object(from, record.key);
        if (!expected) {
            co_return std::unexpected(expected.error());
        }
        auto actual = co_await read_optional(into, record.key);
        if (!actual) {
            co_return std::unexpected(actual.error());
        }
        if (!*actual) {
            report.differences.push_back({record.key, "lease record is missing"});
        } else if (**actual != *expected) {
            report.differences.push_back({record.key, "lease record differs"});
        }
    }
    auto existing = co_await read_state(into, prefix);
    if (!existing) {
        co_return std::unexpected(existing.error());
    }
    for (const auto& [name, bytes] : plan.state) {
        const auto found = existing->find(name);
        if (found == existing->end()) {
            report.differences.push_back({prefix + name, "state key is missing"});
        } else if (found->second != bytes) {
            report.differences.push_back({prefix + name, "state key differs"});
        }
    }
    for (const auto& [name, bytes] : *existing) {
        static_cast<void>(bytes);
        if (!plan.state.contains(name)) {
            report.differences.push_back({prefix + name, "state key is not in the source"});
        }
    }
    co_return result<void>{};
}

} // namespace

runtime::task<repository_transfer_report>
repository::transfer(store& from,
                     store& into,
                     std::filesystem::path identifier,
                     repository_transfer_options options) {
    auto* source = repository_detail::store_access::implementation(from);
    auto* destination = repository_detail::store_access::implementation(into);
    if (!source || !destination) {
        co_return std::unexpected(
            failure(errc::invalid_argument, "store is not open", "repository::transfer"));
    }
    if (source == destination) {
        co_return std::unexpected(failure(errc::invalid_argument,
                                          "a transfer needs two different stores",
                                          "repository::transfer"));
    }
    if (options.batch_objects == 0) {
        co_return std::unexpected(failure(errc::invalid_argument,
                                          "transfer batch must hold at least one object",
                                          "repository::transfer"));
    }
    auto prefix = repository_detail::shared_state_prefix(identifier);
    if (!prefix) {
        co_return std::unexpected(prefix.error());
    }
    const auto from_backend = source->configuration().backend;
    const auto into_backend = destination->configuration().backend;
    if (!from_backend || !into_backend) {
        co_return std::unexpected(
            failure(errc::not_configured, "store backend is unavailable", "repository::transfer"));
    }
    auto plan = co_await plan_transfer(*source, from_backend, *prefix);
    if (!plan) {
        co_return std::unexpected(plan.error());
    }
    auto report = repository_transfer_report{};
    auto walk = transfer_walk{*source, *destination, options, report};
    if (options.mode == repository_transfer_mode::verify) {
        auto walked = co_await walk.run(plan->roots);
        if (!walked) {
            co_return std::unexpected(walked.error());
        }
        auto compared = co_await compare_state(*plan, from_backend, into_backend, *prefix, report);
        if (!compared) {
            co_return std::unexpected(compared.error());
        }
        report.complete = report.differences.empty();
        co_return report;
    }
    destination->begin_object_batch();
    auto walked = co_await walk.run(plan->roots);
    if (walked) {
        walked = co_await destination->transfer_publish();
    }
    if (!walked) {
        // The objects already added stay in memory and every chunk they name is in a pack; the
        // next publication of this store, or a rerun from a fresh open, records them.
        destination->transfer_abandon();
        co_return std::unexpected(walked.error());
    }
    auto committed = co_await destination->commit_object_batch();
    if (!committed) {
        co_return std::unexpected(committed.error());
    }
    auto published = co_await publish_state(*plan, from_backend, into_backend, *prefix, report);
    if (!published) {
        co_return std::unexpected(published.error());
    }
    report.complete = true;
    co_return report;
}

} // namespace liquid
