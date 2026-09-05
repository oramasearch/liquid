#include "liquid/sync.hpp"

#include <algorithm>
#include <charconv>
#include <deque>
#include <map>
#include <optional>
#include <set>
#include <utility>

#include "encoding/cbor.hpp"
#include "liquid/repository.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "repository/store_access.hpp"
#include "repository/sync_access.hpp"
#include "sync/codec.hpp"

namespace liquid {
namespace {

auto server_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

class change_graph {
public:
    explicit change_graph(store& objects) : objects_(&objects) {}

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
        auto bytes = co_await objects_->get(id.value);
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
    std::map<std::string, std::vector<change_id>> parents_;
};

struct reference_authorization {
    std::size_t item_index{};
    sync_ref_update update;
    std::set<std::string> introduced_changes;
};

auto reference_authorizations(repository& subject, store& objects, std::span<const sync_item> items)
    -> runtime::task<std::vector<reference_authorization>> {
    auto current_refs = co_await subject.refs();
    if (!current_refs) {
        co_return std::unexpected(current_refs.error());
    }
    auto current_heads = std::map<std::string, change_id>{};
    for (const auto& current : *current_refs) {
        current_heads.emplace(current.name, current.id);
    }

    auto graph = change_graph{objects};
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
                    std::span<const sync_item> items)
    -> runtime::task<std::vector<sync_ref_outcome>> {
    auto references = co_await reference_authorizations(subject, objects, items);
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
};

sync_server::sync_server(repository& subject, sync_server_options options)
    : sync_server(subject, repository_detail::sync_access::objects(subject), std::move(options)) {}

sync_server::sync_server(repository& subject, store& objects, sync_server_options options)
    : impl_(std::make_unique<impl>(
          impl{&subject,
               &objects,
               std::move(options),
               &repository_detail::sync_access::objects(subject) == &objects})) {}

sync_server::~sync_server() = default;
sync_server::sync_server(sync_server&&) noexcept = default;
sync_server& sync_server::operator=(sync_server&&) noexcept = default;

runtime::task<std::vector<std::byte>> sync_server::handle_push(std::span<const std::byte> request) {
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
    auto outcomes = default_outcomes(incoming->items);
    if (!outcomes) {
        co_return std::unexpected(outcomes.error());
    }
    if (impl_->options.authorize) {
        outcomes = co_await authorize_push(
            impl_->options.authorize, *impl_->subject, *impl_->objects, incoming->items);
        if (!outcomes) {
            co_return std::unexpected(outcomes.error());
        }
    }
    if (incoming->atomic) {
        apply_atomic_rule(*outcomes);
    }
    auto items = accepted_items(incoming->items, *outcomes);
    if (!items.empty()) {
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
    auto frames = sync_detail::decode_pack_frames(request);
    if (!frames) {
        co_return std::unexpected(frames.error());
    }
    for (auto& [id, contents] : *frames) {
        auto stored = co_await repository_detail::store_access::put_verified(
            *impl_->objects, id, std::move(contents));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        if (!same_digest(*stored, id)) {
            co_return std::unexpected(
                server_error(errc::corrupt, "stored object identity changed", to_string(id)));
        }
    }
    co_return result<void>{};
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
    return encoding::cbor::encode(encoding::cbor::value{std::move(fields)});
}

} // namespace liquid
