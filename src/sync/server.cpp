// SPDX-License-Identifier: MIT

#include "liquid/sync.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <deque>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <optional>
#include <random>
#include <set>
#include <system_error>
#include <utility>

#include "encoding/cbor.hpp"
#include "flow_control.hpp"
#include "identity/taap.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/store_access.hpp"
#include "repository/sync_access.hpp"
#include "store/references.hpp"
#include "sync/codec.hpp"

namespace liquid {
namespace {

auto server_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto context_value(std::string_view context, std::string_view name) -> std::string {
    const auto prefix = std::string{name} + "=";
    const auto start = context.find(prefix);
    if (start == std::string_view::npos) {
        return {};
    }
    const auto value_start = start + prefix.size();
    const auto end = context.find(' ', value_start);
    return std::string{context.substr(value_start, end - value_start)};
}

// The quarantine for uploaded objects (liquid#403). `POST /sync/chunks` stages verified objects
// here instead of in the store; a push whose authorization accepts something moves the objects its
// roots reach into the store, and a refused push discards them. Staged bytes are bounded by a
// quota, and an object no push claims expires. Files live in a private directory removed with the
// server.
class upload_staging {
public:
    upload_staging(std::filesystem::path base, std::uint64_t quota, std::chrono::seconds ttl)
        : base_(std::move(base)), quota_(quota), ttl_(ttl) {}

    ~upload_staging() {
        if (!root_.empty()) {
            auto code = std::error_code{};
            std::filesystem::remove_all(root_, code);
        }
    }

    upload_staging(const upload_staging&) = delete;
    auto operator=(const upload_staging&) -> upload_staging& = delete;

    // Stages every frame, or none: the request is refused whole when it would exceed the quota,
    // charged by each frame's declared size, or when a frame does not decode to its digest. Frames
    // are decoded one at a time as they are written, so at most one decoded frame is held in memory
    // (liquid#435).
    auto stage(std::span<const sync_detail::pack_frame> frames) -> result<void> {
        const auto lock = std::scoped_lock{mutex_};
        const auto now = std::chrono::steady_clock::now();
        expire(now);
        auto needed = std::uint64_t{};
        auto keys = std::set<std::string>{};
        for (const auto& frame : frames) {
            const auto name = key(frame.id);
            if (entries_.contains(name) || !keys.insert(name).second) {
                continue;
            }
            needed += frame.raw_size;
        }
        if (needed > quota_ || used_ > quota_ - needed) {
            return std::unexpected(server_error(
                errc::busy,
                "sync upload quota exceeded",
                std::format(
                    "scope=upload limit={} staged={} requested={}", quota_, used_, needed)));
        }
        auto prepared = ensure_root();
        if (!prepared) {
            return std::unexpected(prepared.error());
        }
        auto added = std::vector<std::string>{};
        const auto refuse = [&](error failure) -> result<void> {
            for (const auto& name : added) {
                remove(name);
            }
            return std::unexpected(std::move(failure));
        };
        for (const auto& frame : frames) {
            const auto name = key(frame.id);
            if (const auto found = entries_.find(name); found != entries_.end()) {
                found->second.staged_at = now;
                continue;
            }
            auto contents = sync_detail::decode_pack_frame(frame);
            if (!contents) {
                return refuse(std::move(contents.error()));
            }
            const auto path = root_ / name;
            const auto partial = root_ / (name + ".partial");
            {
                auto output = std::ofstream{partial, std::ios::binary | std::ios::trunc};
                output.write(reinterpret_cast<const char*>(contents->data()),
                             static_cast<std::streamsize>(contents->size()));
                if (!output) {
                    return refuse(server_error(
                        errc::io, "cannot stage an uploaded object", to_string(frame.id)));
                }
            }
            auto code = std::error_code{};
            std::filesystem::rename(partial, path, code);
            if (code) {
                return refuse(server_error(errc::io, code.message(), to_string(frame.id)));
            }
            entries_.emplace(name, entry{.size = contents->size(), .staged_at = now});
            used_ += contents->size();
            added.push_back(name);
        }
        return {};
    }

    auto contains(const digest& id) -> bool {
        const auto lock = std::scoped_lock{mutex_};
        return entries_.contains(key(id));
    }

    auto read(const digest& id) -> result<std::vector<std::byte>> {
        const auto lock = std::scoped_lock{mutex_};
        const auto name = key(id);
        if (!entries_.contains(name)) {
            return std::unexpected(
                server_error(errc::not_found, "object is not staged", to_string(id)));
        }
        auto input = std::ifstream{root_ / name, std::ios::binary};
        if (!input) {
            return std::unexpected(
                server_error(errc::io, "cannot read a staged object", to_string(id)));
        }
        auto bytes = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
        auto output = std::vector<std::byte>(bytes.size());
        std::ranges::transform(
            bytes, output.begin(), [](char value) { return static_cast<std::byte>(value); });
        auto actual = hash(id.alg, output);
        if (!actual || actual->alg != id.alg || actual->bytes != id.bytes) {
            return std::unexpected(
                server_error(errc::corrupt, "staged object digest changed", to_string(id)));
        }
        return output;
    }

    void discard(const digest& id) {
        const auto lock = std::scoped_lock{mutex_};
        remove(key(id));
    }

    auto staged_bytes() -> std::uint64_t {
        const auto lock = std::scoped_lock{mutex_};
        return used_;
    }

private:
    struct entry {
        std::uint64_t size{};
        std::chrono::steady_clock::time_point staged_at;
    };

    static auto key(const digest& id) -> std::string {
        auto output = std::format("{:02x}-", static_cast<unsigned>(id.alg));
        for (const auto value : id.bytes) {
            output += std::format("{:02x}", std::to_integer<unsigned>(value));
        }
        return output;
    }

    // Called with the lock held.
    void remove(const std::string& name) {
        const auto found = entries_.find(name);
        if (found == entries_.end()) {
            return;
        }
        used_ -= found->second.size;
        entries_.erase(found);
        auto code = std::error_code{};
        std::filesystem::remove(root_ / name, code);
    }

    // Called with the lock held.
    void expire(std::chrono::steady_clock::time_point now) {
        auto expired = std::vector<std::string>{};
        for (const auto& [name, value] : entries_) {
            if (now - value.staged_at >= ttl_) {
                expired.push_back(name);
            }
        }
        for (const auto& name : expired) {
            remove(name);
        }
    }

    // Called with the lock held. Creates the private quarantine directory on first use.
    auto ensure_root() -> result<void> {
        if (!root_.empty()) {
            return {};
        }
        auto code = std::error_code{};
        auto base = base_.empty() ? std::filesystem::temp_directory_path(code) : base_;
        if (code) {
            return std::unexpected(server_error(errc::io, code.message(), "sync upload staging"));
        }
        std::filesystem::create_directories(base, code);
        auto random = std::random_device{};
        for (auto attempt = 0; attempt < 64; ++attempt) {
            const auto candidate =
                base / std::format("liquid-sync-staging-{:08x}{:08x}", random(), random());
            code.clear();
            if (std::filesystem::create_directory(candidate, code)) {
                std::filesystem::permissions(candidate,
                                             std::filesystem::perms::owner_all,
                                             std::filesystem::perm_options::replace,
                                             code);
                root_ = candidate;
                return {};
            }
        }
        return std::unexpected(server_error(
            errc::io, "cannot create the sync upload staging directory", base.string()));
    }

    std::filesystem::path base_;
    std::filesystem::path root_;
    std::uint64_t quota_{};
    std::chrono::seconds ttl_{};
    std::uint64_t used_{};
    std::mutex mutex_;
    std::map<std::string, entry> entries_;
};

// Reads an object from the store, or from the quarantine when the store does not have it.
auto read_object(store& destination, upload_staging& staged, const digest& id)
    -> runtime::task<std::vector<std::byte>> {
    auto present = co_await destination.contains(id);
    if (!present) {
        co_return std::unexpected(present.error());
    }
    if (*present) {
        co_return co_await destination.get(id);
    }
    co_return staged.read(id);
}

// The staged objects reachable from `roots`, children before parents. The walk follows the same
// edges as `sync_session::send`, so it reaches every object a push uploaded for those roots;
// payload chunks are named without being read, and objects in neither place are skipped.
auto staged_closure(store& destination, upload_staging& staged, std::span<const digest> roots)
    -> runtime::task<std::vector<digest>> {
    if (staged.staged_bytes() == 0) {
        co_return std::vector<digest>{};
    }
    struct pending_object {
        digest id;
        bool payload{};
    };
    auto pending = std::deque<pending_object>{};
    for (const auto& root : roots) {
        pending.push_back({root, false});
    }
    auto visited = std::set<std::string>{};
    auto output = std::vector<digest>{};
    auto registry = store_detail::reference_registry{};
    while (!pending.empty()) {
        const auto item = pending.front();
        pending.pop_front();
        if (!visited.insert(to_string(item.id)).second) {
            continue;
        }
        const auto is_staged = staged.contains(item.id);
        if (is_staged) {
            output.push_back(item.id);
        }
        if (item.payload) {
            continue;
        }
        auto bytes = result<std::vector<std::byte>>{};
        if (is_staged) {
            bytes = staged.read(item.id);
        } else {
            auto present = co_await destination.contains(item.id);
            if (!present) {
                co_return std::unexpected(present.error());
            }
            if (!*present) {
                continue;
            }
            bytes = co_await destination.get(item.id);
        }
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        const auto kind = store_detail::detect_object_kind(*bytes);
        if (kind == "blob") {
            auto value = objects::decode_blob(*bytes);
            if (!value) {
                co_return std::unexpected(value.error());
            }
            for (const auto& chunk : value->chunks) {
                pending.push_back({chunk, true});
            }
            for (const auto& entry : value->format_entries) {
                pending.push_back({entry, false});
            }
            continue;
        }
        const auto references = registry.extract(kind, *bytes);
        const auto payload = kind == "chunk-list";
        for (const auto& reference : references.digests) {
            pending.push_back({reference, payload});
        }
    }
    std::ranges::reverse(output);
    co_return output;
}

// Moves the staged objects a push's roots reach into the store, children first.
auto admit_staged(store& destination, upload_staging& staged, std::span<const digest> roots)
    -> runtime::task<void> {
    auto closure = co_await staged_closure(destination, staged, roots);
    if (!closure) {
        co_return std::unexpected(closure.error());
    }
    for (const auto& id : *closure) {
        auto bytes = staged.read(id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto stored = co_await repository_detail::store_access::put_verified(
            destination, id, std::move(*bytes));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        if (!same_digest(*stored, id)) {
            co_return std::unexpected(
                server_error(errc::corrupt, "stored object identity changed", to_string(id)));
        }
        staged.discard(id);
    }
    co_return result<void>{};
}

// Discards the staged objects a refused push's roots reach. A walk failure leaves them to expire.
auto discard_staged(store& destination, upload_staging& staged, std::span<const digest> roots)
    -> runtime::task<void> {
    auto closure = co_await staged_closure(destination, staged, roots);
    if (closure) {
        for (const auto& id : *closure) {
            staged.discard(id);
        }
    }
    co_return result<void>{};
}

// Returns `value` after discarding what the refused push uploaded: a refused push leaves nothing
// behind (liquid#403).
auto refuse_push(store& destination,
                 upload_staging& staged,
                 std::span<const digest> roots,
                 error value) -> runtime::task<std::vector<std::byte>> {
    static_cast<void>(co_await discard_staged(destination, staged, roots));
    co_return std::unexpected(std::move(value));
}

auto item_failure(std::size_t index, error value) -> error {
    value.context = "item=" + std::to_string(index) + ": " + value.context;
    return value;
}

// The identity chain a push carries for `taap_id`, when it carries one.
auto carried_chain(std::span<const sync_item> items, std::string_view taap_id)
    -> std::optional<std::vector<identity_document>> {
    for (const auto& item : items) {
        if (item.kind != sync_item_kind::identity || item.author.taap_id != taap_id) {
            continue;
        }
        auto chain = decode_identity_chain(item.body);
        if (chain && !chain->empty() && chain->front().subject.taap_id == taap_id) {
            return std::move(*chain);
        }
    }
    return std::nullopt;
}

// True only when `by`'s key, bound to its name by the stored chain or the one the push carries,
// made `signed_value` over `body`. Import repeats the full checks; this only decides whose flow
// budget an item may spend.
auto signed_by(const actor& by,
               std::span<const sync_item> items,
               std::string_view artifact_type,
               std::span<const std::byte> body,
               const signature& signed_value) -> result<void> {
    auto stored = verify(by, artifact_type, body, signed_value);
    if (stored && *stored) {
        return {};
    }
    if (auto chain = carried_chain(items, by.taap_id)) {
        auto carried = verify(by, *chain, artifact_type, body, signed_value);
        if (carried && *carried) {
            return {};
        }
        if (!carried) {
            return std::unexpected(carried.error());
        }
    }
    if (!stored) {
        return std::unexpected(stored.error());
    }
    return std::unexpected(
        server_error(errc::unauthorized, "signature does not verify", by.taap_id));
}

// Verifies the author an item names before that author's flow budget is spent (liquid#403):
// a claimed author is trusted only when the item is signed by that author's key.
auto verify_claimed_author(const sync_item& item, std::span<const sync_item> items)
    -> result<void> {
    switch (item.kind) {
    case sync_item_kind::identity: {
        auto chain = decode_identity_chain(item.body);
        if (!chain) {
            return std::unexpected(chain.error());
        }
        if (chain->empty() || chain->front().subject.taap_id != item.author.taap_id) {
            return std::unexpected(
                server_error(errc::corrupt, "identity chain does not match its frame author"));
        }
        auto verified = verify_chain(*chain);
        if (!verified) {
            return std::unexpected(verified.error());
        }
        return {};
    }
    case sync_item_kind::drip: {
        auto signed_value = identity_detail::parse_signature(item.signature);
        if (!signed_value) {
            return std::unexpected(signed_value.error());
        }
        return signed_by(item.author, items, "drip", item.body, *signed_value);
    }
    case sync_item_kind::change: {
        auto value = objects::decode_change(item.body, item.reference.value.alg);
        if (!value) {
            return std::unexpected(value.error());
        }
        auto body = objects::encode_change_body(*value);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto signed_value = identity_detail::parse_signature(value->sig);
        if (!signed_value) {
            return std::unexpected(signed_value.error());
        }
        return signed_by(item.author, items, "change", *body, *signed_value);
    }
    default:
        return std::unexpected(
            server_error(errc::unauthorized, "this metadata frame kind cannot name an author"));
    }
}

class change_graph {
public:
    change_graph(store& objects, upload_staging& staged) : objects_(&objects), staged_(&staged) {}

    auto reachable(change_id head) -> runtime::task<std::set<std::string>> {
        auto output = std::set<std::string>{};
        auto pending = std::deque<change_id>{head};
        while (!pending.empty()) {
            const auto current = pending.front();
            pending.pop_front();
            const auto key = to_string(current.value);
            if (!output.insert(key).second) {
                continue;
            }
            auto parents = co_await load_parents(current);
            if (!parents) {
                co_return std::unexpected(parents.error());
            }
            pending.insert(pending.end(), parents->begin(), parents->end());
        }
        co_return output;
    }

private:
    auto load_parents(change_id id) -> runtime::task<std::vector<change_id>> {
        const auto key = to_string(id.value);
        if (const auto found = parents_.find(key); found != parents_.end()) {
            co_return found->second;
        }
        auto bytes = co_await read_object(*objects_, *staged_, id.value);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto value = objects::decode_change(*bytes, id.value.alg);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        auto parents = std::vector<change_id>{};
        parents.reserve(value->parents.size());
        for (const auto& parent : value->parents) {
            parents.push_back(parent.id);
        }
        parents_.emplace(key, parents);
        co_return parents;
    }

    store* objects_{};
    upload_staging* staged_{};
    std::map<std::string, std::vector<change_id>> parents_;
};

struct reference_authorization {
    std::size_t item_index{};
    sync_ref_update update;
    std::set<std::string> introduced_changes;
};

auto reference_authorizations(repository& subject,
                              store& objects,
                              upload_staging& staged,
                              std::span<const sync_item> items)
    -> runtime::task<std::vector<reference_authorization>> {
    auto current_refs = co_await subject.refs();
    if (!current_refs) {
        co_return std::unexpected(current_refs.error());
    }
    auto current_heads = std::map<std::string, change_id>{};
    for (const auto& current : *current_refs) {
        current_heads.emplace(current.name, current.id);
    }

    auto graph = change_graph{objects, staged};
    auto output = std::vector<reference_authorization>{};
    auto proposed_names = std::set<std::string>{};
    for (auto index = std::size_t{}; index < items.size(); ++index) {
        const auto& item = items[index];
        if (item.kind != sync_item_kind::reference) {
            continue;
        }
        if (!proposed_names.insert(item.name).second) {
            co_return std::unexpected(server_error(
                errc::invalid_argument, "push contains duplicate reference updates", item.name));
        }
        auto old_head = std::optional<change_id>{};
        if (const auto found = current_heads.find(item.name); found != current_heads.end()) {
            old_head = found->second;
        }
        auto new_reachable = co_await graph.reachable(item.reference);
        if (!new_reachable) {
            co_return std::unexpected(new_reachable.error());
        }
        auto old_reachable = std::set<std::string>{};
        if (old_head) {
            auto reachable = co_await graph.reachable(*old_head);
            if (!reachable) {
                co_return std::unexpected(reachable.error());
            }
            old_reachable = std::move(*reachable);
        }
        const auto fast_forward = !old_head || new_reachable->contains(to_string(old_head->value));
        for (const auto& old_change : old_reachable) {
            new_reachable->erase(old_change);
        }
        output.push_back(reference_authorization{
            .item_index = index,
            .update = sync_ref_update{item.name, old_head, item.reference, fast_forward},
            .introduced_changes = std::move(*new_reachable),
        });
    }
    co_return output;
}

auto refused_outcomes(std::span<const reference_authorization> references, std::string reason)
    -> std::vector<sync_ref_outcome> {
    auto output = std::vector<sync_ref_outcome>{};
    output.reserve(references.size());
    for (const auto& reference : references) {
        output.push_back(
            sync_ref_outcome{reference.update.name, sync_ref_outcome_kind::refused, reason});
    }
    return output;
}

auto invoke_authorizer(sync_authorizer& authorize,
                       const sync_item& item,
                       std::span<const sync_ref_update> updates)
    -> runtime::task<std::vector<sync_ref_verdict>> {
    try {
        co_return co_await authorize(item, updates);
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            server_error(errc::internal, exception.what(), "sync authorizer"));
    } catch (...) {
        co_return std::unexpected(
            server_error(errc::internal, "unknown exception", "sync authorizer"));
    }
}

auto apply_verdicts(std::span<const sync_ref_update> updates,
                    std::span<const sync_ref_verdict> verdicts,
                    std::vector<sync_ref_outcome>& outcomes) -> result<void> {
    if (updates.size() != verdicts.size()) {
        return std::unexpected(
            server_error(errc::internal, "sync authorizer returned the wrong number of verdicts"));
    }
    auto seen = std::set<std::string>{};
    for (const auto& verdict : verdicts) {
        if (!seen.insert(verdict.name).second ||
            std::ranges::find(updates, verdict.name, &sync_ref_update::name) == updates.end()) {
            return std::unexpected(server_error(
                errc::internal, "sync authorizer returned an unknown reference", verdict.name));
        }
        if (verdict.verdict != sync_ref_verdict_kind::accepted &&
            verdict.verdict != sync_ref_verdict_kind::refused) {
            return std::unexpected(server_error(
                errc::internal, "sync authorizer returned an invalid verdict", verdict.name));
        }
        if (verdict.verdict != sync_ref_verdict_kind::refused) {
            continue;
        }
        const auto outcome = std::ranges::find(outcomes, verdict.name, &sync_ref_outcome::name);
        if (outcome == outcomes.end()) {
            return std::unexpected(server_error(
                errc::internal, "sync authorizer reference context is missing", verdict.name));
        }
        if (outcome->outcome == sync_ref_outcome_kind::applied) {
            outcome->outcome = sync_ref_outcome_kind::refused;
            outcome->reason = verdict.reason;
        }
    }
    return {};
}

auto authorize_push(sync_authorizer& authorize,
                    repository& subject,
                    store& objects,
                    upload_staging& staged,
                    std::span<const sync_item> items)
    -> runtime::task<std::vector<sync_ref_outcome>> {
    auto references = co_await reference_authorizations(subject, objects, staged, items);
    if (!references) {
        co_return std::unexpected(references.error());
    }
    auto outcomes = std::vector<sync_ref_outcome>{};
    outcomes.reserve(references->size());
    for (const auto& reference : *references) {
        outcomes.push_back(sync_ref_outcome{reference.update.name});
    }
    for (auto index = std::size_t{}; index < items.size(); ++index) {
        const auto& item = items[index];
        if (item.kind == sync_item_kind::change) {
            auto updates = std::vector<sync_ref_update>{};
            const auto id = to_string(item.reference.value);
            for (const auto& reference : *references) {
                if (reference.introduced_changes.contains(id)) {
                    updates.push_back(reference.update);
                }
            }
            if (updates.empty()) {
                continue;
            }
            auto verdicts = co_await invoke_authorizer(authorize, item, updates);
            if (!verdicts) {
                co_return refused_outcomes(*references, verdicts.error().message);
            }
            auto applied = apply_verdicts(updates, *verdicts, outcomes);
            if (!applied) {
                co_return std::unexpected(applied.error());
            }
            continue;
        }
        if (item.kind != sync_item_kind::reference) {
            continue;
        }
        const auto found =
            std::ranges::find(*references, index, &reference_authorization::item_index);
        if (found == references->end()) {
            co_return std::unexpected(
                server_error(errc::internal, "reference authorization context is missing"));
        }
        auto updates = std::span{&found->update, std::size_t{1}};
        auto verdicts = co_await invoke_authorizer(authorize, item, updates);
        if (!verdicts) {
            co_return refused_outcomes(*references, verdicts.error().message);
        }
        auto applied = apply_verdicts(updates, *verdicts, outcomes);
        if (!applied) {
            co_return std::unexpected(applied.error());
        }
    }
    co_return outcomes;
}

auto default_outcomes(std::span<const sync_item> items) -> result<std::vector<sync_ref_outcome>> {
    auto names = std::set<std::string>{};
    auto output = std::vector<sync_ref_outcome>{};
    for (const auto& item : items) {
        if (item.kind != sync_item_kind::reference) {
            continue;
        }
        if (!names.insert(item.name).second) {
            return std::unexpected(server_error(
                errc::invalid_argument, "push contains duplicate reference updates", item.name));
        }
        output.push_back(sync_ref_outcome{item.name});
    }
    return output;
}

void apply_atomic_rule(std::vector<sync_ref_outcome>& outcomes) {
    const auto refused = std::ranges::any_of(outcomes, [](const auto& outcome) {
        return outcome.outcome == sync_ref_outcome_kind::refused;
    });
    if (!refused) {
        return;
    }
    for (auto& outcome : outcomes) {
        if (outcome.outcome == sync_ref_outcome_kind::applied) {
            outcome.outcome = sync_ref_outcome_kind::refused;
            outcome.reason = "atomic push refused because another reference was refused";
        }
    }
}

auto accepted_items(std::span<const sync_item> items, std::span<const sync_ref_outcome> outcomes)
    -> std::vector<sync_item> {
    const auto has_references = !outcomes.empty();
    const auto has_accepted_reference = std::ranges::any_of(outcomes, [](const auto& outcome) {
        return outcome.outcome == sync_ref_outcome_kind::applied;
    });
    if (has_references && !has_accepted_reference) {
        return {};
    }
    auto output = std::vector<sync_item>{};
    output.reserve(items.size());
    for (const auto& item : items) {
        if (item.kind != sync_item_kind::reference) {
            output.push_back(item);
            continue;
        }
        const auto outcome = std::ranges::find(outcomes, item.name, &sync_ref_outcome::name);
        if (outcome != outcomes.end() && outcome->outcome == sync_ref_outcome_kind::applied) {
            output.push_back(item);
        }
    }
    return output;
}

} // namespace

struct sync_server::impl {
    repository* subject{};
    store* objects{};
    sync_server_options options;
    bool matching_store{};
    std::unique_ptr<flow_detail::controller> flow_limits;
    std::unique_ptr<upload_staging> staged;
};

sync_server::sync_server(repository& subject, sync_server_options options)
    : sync_server(subject, repository_detail::sync_access::objects(subject), std::move(options)) {}

sync_server::sync_server(repository& subject, store& objects, sync_server_options options)
    : impl_(std::make_unique<impl>(
          impl{&subject,
               &objects,
               options,
               &repository_detail::sync_access::objects(subject) == &objects,
               std::make_unique<flow_detail::controller>(subject.flow_configuration()),
               std::make_unique<upload_staging>(
                   options.staging_directory, options.upload_quota_bytes, options.upload_ttl)})) {}

sync_server::~sync_server() = default;
sync_server::sync_server(sync_server&&) noexcept = default;
sync_server& sync_server::operator=(sync_server&&) noexcept = default;

runtime::task<std::vector<std::byte>> sync_server::handle_push(std::span<const std::byte> request) {
    if (!impl_) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    co_return co_await handle_push(request, std::string{impl_->options.pusher});
}

runtime::task<std::vector<std::byte>> sync_server::handle_push(std::span<const std::byte> request,
                                                               std::string pusher) {
    if (!impl_ || !impl_->subject || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    auto incoming = sync_detail::decode_metadata(request);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    if (!incoming->outcomes.empty()) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "push request contains reference outcomes"));
    }
    // The push rate limit (liquid#508, liquid#403). An authenticated push is charged whole to the
    // pusher, never to the authors its records name: a pusher cannot spend another person's budget
    // by relaying their records. An unauthenticated push charges each item to the author it names,
    // once the item is proven to be that author's, and an item that names no author to the shared
    // `unauthenticated_pusher` bucket. A claimed author is verified either way, so a forged one is
    // refused before anything is spent.
    const auto authenticated = !pusher.empty();
    const auto pusher_bucket =
        authenticated ? std::move(pusher) : std::string{unauthenticated_pusher};
    auto& objects = *impl_->objects;
    auto& staged = *impl_->staged;
    const auto roots = std::span<const digest>{incoming->objects};
    for (auto index = std::size_t{}; index < incoming->items.size(); ++index) {
        const auto& item = incoming->items[index];
        auto bucket = pusher_bucket;
        if (!item.author.taap_id.empty()) {
            auto verified = verify_claimed_author(item, incoming->items);
            if (!verified) {
                co_return co_await refuse_push(
                    objects, staged, roots, item_failure(index, std::move(verified.error())));
            }
            if (!authenticated) {
                bucket = item.author.taap_id;
            }
        }
        auto document = item.directory;
        if (std::ranges::all_of(document, [](std::byte value) { return value == std::byte{}; })) {
            document = item.repository;
        }
        auto admitted = impl_->flow_limits->admit(
            std::move(bucket), flow_detail::node_key(document), item.body.size());
        if (!admitted) {
            co_return co_await refuse_push(objects, staged, roots, std::move(admitted.error()));
        }
    }
    auto outcomes = default_outcomes(incoming->items);
    if (!outcomes) {
        co_return co_await refuse_push(objects, staged, roots, std::move(outcomes.error()));
    }
    if (impl_->options.authorize) {
        outcomes = co_await authorize_push(
            impl_->options.authorize, *impl_->subject, objects, staged, incoming->items);
        if (!outcomes) {
            co_return co_await refuse_push(objects, staged, roots, std::move(outcomes.error()));
        }
    }
    if (incoming->atomic) {
        apply_atomic_rule(*outcomes);
    }
    auto items = accepted_items(incoming->items, *outcomes);
    if (items.empty()) {
        static_cast<void>(co_await discard_staged(objects, staged, roots));
    } else {
        // Authorized: the uploaded objects the push reaches enter the store before its metadata.
        auto admitted = co_await admit_staged(objects, staged, roots);
        if (!admitted) {
            co_return std::unexpected(admitted.error());
        }
        auto imported = co_await impl_->subject->import_drips(items);
        if (!imported) {
            co_return std::unexpected(imported.error());
        }
    }
    co_return sync_detail::encode_metadata(sync_detail::metadata_document{
        .cursor = incoming->cursor,
        .outcomes = std::move(*outcomes),
    });
}

runtime::task<std::vector<std::byte>> sync_server::handle_pull(std::span<const std::byte> request) {
    if (!impl_ || !impl_->subject || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    if (impl_->options.batch_items == 0) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server batch size must be positive"));
    }
    auto incoming = sync_detail::decode_metadata(request);
    if (!incoming) {
        co_return std::unexpected(incoming.error());
    }
    if (!incoming->items.empty() || !incoming->objects.empty() || !incoming->outcomes.empty() ||
        incoming->atomic) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "pull request contains metadata or objects"));
    }
    auto outgoing = sync_detail::metadata_document{};
    auto cursor = incoming->cursor;
    auto page = std::vector<sync_item>(impl_->options.batch_items);
    for (;;) {
        auto exported = co_await impl_->subject->export_drips(cursor, page);
        if (!exported) {
            co_return std::unexpected(exported.error());
        }
        outgoing.items.insert(outgoing.items.end(),
                              std::make_move_iterator(page.begin()),
                              std::make_move_iterator(page.begin() + exported->count));
        outgoing.objects.insert(
            outgoing.objects.end(), exported->objects.begin(), exported->objects.end());
        cursor = exported->cursor;
        if (exported->done) {
            break;
        }
    }
    outgoing.cursor = std::move(cursor);
    co_return sync_detail::encode_metadata(outgoing);
}

runtime::task<std::vector<std::byte>>
sync_server::handle_find_missing(std::span<const std::byte> request) {
    if (!impl_ || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    auto requested = sync_detail::decode_object_list(request);
    if (!requested) {
        co_return std::unexpected(requested.error());
    }
    auto missing = std::vector<digest>{};
    for (const auto& id : *requested) {
        auto present = co_await impl_->objects->contains(id);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (!*present) {
            missing.push_back(id);
        }
    }
    co_return sync_detail::encode_object_list(missing);
}

runtime::task<void> sync_server::handle_chunks(std::span<const std::byte> request) {
    if (!impl_ || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    // Each frame header is checked, and the request's declared size bounded, before anything is
    // decoded (liquid#435). Nothing enters the store here: the objects are staged until a push that
    // reaches them is authorized (liquid#403).
    auto frames = sync_detail::parse_pack_frames(request);
    if (!frames) {
        co_return std::unexpected(frames.error());
    }
    auto absent = std::vector<sync_detail::pack_frame>{};
    absent.reserve(frames->size());
    for (const auto& frame : *frames) {
        auto present = co_await impl_->objects->contains(frame.id);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (!*present) {
            absent.push_back(frame);
        }
    }
    co_return impl_->staged->stage(absent);
}

runtime::task<std::uint64_t>
sync_server::handle_chunks(digest id, std::uint64_t offset, std::span<std::byte> output) {
    if (!impl_ || !impl_->objects || !impl_->matching_store) {
        co_return std::unexpected(
            server_error(errc::invalid_argument, "sync server repository and store do not match"));
    }
    co_return co_await impl_->objects->read(id, offset, output);
}

result<std::vector<std::byte>> sync_server::encode_error(const error& value) {
    auto context = value.context;
    auto item = std::optional<std::uint64_t>{};
    if (context.starts_with("item=")) {
        const auto separator = context.find(':');
        const auto number = std::string_view{context}.substr(5, separator - 5);
        auto parsed_item = std::uint64_t{};
        const auto parsed =
            std::from_chars(number.data(), number.data() + number.size(), parsed_item);
        if (parsed.ec == std::errc{} && parsed.ptr == number.data() + number.size()) {
            item = parsed_item;
            context =
                separator == std::string::npos ? std::string{} : context.substr(separator + 1);
            if (context.starts_with(' ')) {
                context.erase(0, 1);
            }
        }
    }
    auto fields = encoding::cbor::value::map{
        {"v", encoding::cbor::value{std::uint64_t{1}}},
        {"code", std::string{to_string(value.code)}},
        {"message", value.message},
        {"context", std::move(context)},
    };
    if (item) {
        fields.emplace_back("item", *item);
    }
    const auto retry_text = context_value(value.context, "retry_after_ms");
    auto retry_after_ms = std::uint64_t{};
    const auto parsed_retry =
        std::from_chars(retry_text.data(), retry_text.data() + retry_text.size(), retry_after_ms);
    if (!retry_text.empty() && parsed_retry.ec == std::errc{} &&
        parsed_retry.ptr == retry_text.data() + retry_text.size()) {
        fields.emplace_back("retry_after_ms", retry_after_ms);
    }
    const auto scope = context_value(value.context, "scope");
    if (!scope.empty()) {
        fields.emplace_back("scope", scope);
    }
    return encoding::cbor::encode(encoding::cbor::value{std::move(fields)});
}

} // namespace liquid
