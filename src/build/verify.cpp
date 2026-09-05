#include "liquid/build.hpp"

#include <algorithm>
#include <chrono>
#include <limits>
#include <map>
#include <ranges>
#include <set>

#include "build/cache.hpp"
#include "build/executor.hpp"
#include "build/repository_access.hpp"
#include "build/verify.hpp"
#include "build/wrap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace liquid::build {
namespace {

auto verification_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

runtime::task<void> hydrate_blob(repository& source, digest id, std::set<std::string>& seen) {
    if (!seen.insert(to_string(id)).second) {
        co_return liquid::result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto value = objects::decode_blob(*bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    if (value->content.alg == algorithm::blake3 || value->content.alg == algorithm::sha256 ||
        value->content.alg == algorithm::blake3_keyed) {
        auto contents = co_await repository_detail::build_access::get(source, value->content);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        co_return liquid::result<void>{};
    }
    for (const auto& chunk : value->chunks) {
        auto contents = co_await repository_detail::build_access::get(source, chunk);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<void> hydrate_tree(repository& source, digest id, std::set<std::string>& seen) {
    if (!seen.insert(to_string(id)).second) {
        co_return liquid::result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto value = objects::decode_tree(*bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    for (const auto& entry : value->entries) {
        auto hydrated = liquid::result<void>{};
        if (entry.kind == node_kind::blob) {
            hydrated = co_await hydrate_blob(source, entry.ref, seen);
        } else if (entry.kind == node_kind::tree || entry.kind == node_kind::repo) {
            hydrated = co_await hydrate_tree(source, entry.ref, seen);
        }
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<void> hydrate_replay_graph(repository& source,
                                         const action_result& recorded,
                                         const objects::action_record& action) {
    auto seen = std::set<std::string>{};
    auto inputs = co_await hydrate_tree(source, action.input.inputs, seen);
    if (!inputs) {
        co_return std::unexpected(inputs.error());
    }
    auto toolchain = co_await hydrate_tree(source, action.input.toolchain, seen);
    if (!toolchain) {
        co_return std::unexpected(toolchain.error());
    }
    for (const auto& output : recorded.outputs) {
        auto hydrated = co_await hydrate_blob(source, output.blob, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    auto stdout_output = co_await hydrate_blob(source, recorded.stdout_output, seen);
    if (!stdout_output) {
        co_return std::unexpected(stdout_output.error());
    }
    auto stderr_output = co_await hydrate_blob(source, recorded.stderr_output, seen);
    if (!stderr_output) {
        co_return std::unexpected(stderr_output.error());
    }
    co_return liquid::result<void>{};
}

runtime::task<std::vector<std::byte>> read_blob(repository& source, const blob& value) {
    if (value.size > std::numeric_limits<std::size_t>::max()) {
        co_return std::unexpected(
            verification_error(errc::unsupported, "output is too large to explain"));
    }
    if (value.content.alg == algorithm::blake3 || value.content.alg == algorithm::sha256 ||
        value.content.alg == algorithm::blake3_keyed) {
        auto contents = co_await repository_detail::build_access::get(source, value.content);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        if (contents->size() != value.size) {
            co_return std::unexpected(
                verification_error(errc::corrupt, "output length differs from its blob record"));
        }
        co_return contents;
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(value.size));
    for (const auto& chunk : value.chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != value.size) {
        co_return std::unexpected(
            verification_error(errc::corrupt, "output length differs from its blob record"));
    }
    co_return output;
}

runtime::task<void> add_byte_difference(repository& source,
                                        reproducibility_difference& difference) {
    if (!difference.cached || !difference.reexecuted) {
        co_return liquid::result<void>{};
    }
    auto cached_record = co_await repository_detail::build_access::get(source, *difference.cached);
    auto replayed_record =
        co_await repository_detail::build_access::get(source, *difference.reexecuted);
    if (!cached_record || !replayed_record) {
        co_return std::unexpected(!cached_record ? cached_record.error() : replayed_record.error());
    }
    auto cached_blob = objects::decode_blob(*cached_record);
    auto replayed_blob = objects::decode_blob(*replayed_record);
    if (!cached_blob || !replayed_blob) {
        co_return std::unexpected(!cached_blob ? cached_blob.error() : replayed_blob.error());
    }
    auto cached_bytes = co_await read_blob(source, *cached_blob);
    auto replayed_bytes = co_await read_blob(source, *replayed_blob);
    if (!cached_bytes || !replayed_bytes) {
        co_return std::unexpected(!cached_bytes ? cached_bytes.error() : replayed_bytes.error());
    }
    const auto mismatch = std::ranges::mismatch(*cached_bytes, *replayed_bytes);
    const auto cached_end = mismatch.in1 == cached_bytes->end();
    const auto replayed_end = mismatch.in2 == replayed_bytes->end();
    if (!cached_end || !replayed_end) {
        difference.byte_offset =
            static_cast<std::uint64_t>(std::distance(cached_bytes->begin(), mismatch.in1));
        if (!cached_end) {
            difference.cached_byte = std::to_integer<std::uint8_t>(*mismatch.in1);
        }
        if (!replayed_end) {
            difference.reexecuted_byte = std::to_integer<std::uint8_t>(*mismatch.in2);
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<void> add_digest_difference(repository& source,
                                          std::vector<reproducibility_difference>& differences,
                                          std::string output,
                                          std::optional<digest> cached,
                                          std::optional<digest> replayed,
                                          bool explain) {
    if (cached && replayed && same_digest(*cached, *replayed)) {
        co_return liquid::result<void>{};
    }
    auto difference = reproducibility_difference{
        .output = std::move(output), .cached = cached, .reexecuted = replayed};
    if (cached && replayed) {
        auto explained = co_await add_byte_difference(source, difference);
        if (!explained) {
            co_return std::unexpected(explained.error());
        }
        if (!difference.byte_offset) {
            co_return liquid::result<void>{};
        }
        if (!explain) {
            difference.byte_offset = {};
            difference.cached_byte = {};
            difference.reexecuted_byte = {};
        }
    }
    differences.push_back(std::move(difference));
    co_return liquid::result<void>{};
}

runtime::task<std::vector<reproducibility_difference>> compare(repository& source,
                                                               const action_result& cached,
                                                               const action_result& replayed,
                                                               bool explain) {
    auto differences = std::vector<reproducibility_difference>{};
    if (cached.exit != replayed.exit) {
        differences.push_back({.output = "exit-status"});
    }
    auto cached_outputs = std::map<std::string, digest>{};
    auto replayed_outputs = std::map<std::string, digest>{};
    auto stdout_difference = co_await add_digest_difference(
        source, differences, "stdout", cached.stdout_output, replayed.stdout_output, explain);
    if (!stdout_difference) {
        co_return std::unexpected(stdout_difference.error());
    }
    auto stderr_difference = co_await add_digest_difference(
        source, differences, "stderr", cached.stderr_output, replayed.stderr_output, explain);
    if (!stderr_difference) {
        co_return std::unexpected(stderr_difference.error());
    }
    for (const auto& output : cached.outputs) {
        cached_outputs.emplace(output.path, output.blob);
    }
    for (const auto& output : replayed.outputs) {
        replayed_outputs.emplace(output.path, output.blob);
    }
    auto names = std::set<std::string>{};
    for (const auto& [name, value] : cached_outputs) {
        static_cast<void>(value);
        names.insert(name);
    }
    for (const auto& [name, value] : replayed_outputs) {
        static_cast<void>(value);
        names.insert(name);
    }
    for (const auto& name : names) {
        const auto cached_value = cached_outputs.find(name);
        const auto replayed_value = replayed_outputs.find(name);
        auto difference = co_await add_digest_difference(
            source,
            differences,
            name,
            cached_value == cached_outputs.end() ? std::nullopt
                                                 : std::optional{cached_value->second},
            replayed_value == replayed_outputs.end() ? std::nullopt
                                                     : std::optional{replayed_value->second},
            explain);
        if (!difference) {
            co_return std::unexpected(difference.error());
        }
    }
    co_return differences;
}

auto selected(const objects::action_index_entry& entry,
              const action_result& value,
              const verify_options& options,
              std::uint32_t rate) -> bool {
    switch (options.selection) {
    case verify_selection::sample:
        return !entry.verification && !entry.divergent && (entry.sequence - 1) % rate == 0;
    case verify_selection::all:
        return true;
    case verify_selection::changed:
        return !entry.verification && !entry.divergent;
    case verify_selection::action:
        return options.action && same_digest(value.action, *options.action);
    }
    return false;
}

struct graph_query_request {
    action_input input;
    digest sandbox;
    std::vector<observed_input> manifest;
};

struct graph_query_state {
    build_detail::disk_cache* cache{};
    std::map<std::string, std::pair<std::uint64_t, graph_query_request>> requests;
};

auto graph_bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto graph_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

runtime::task<std::vector<std::byte>> clean_action_result_query(query_context& context,
                                                                std::vector<std::byte> arguments,
                                                                graph_query_state* state) {
    const auto candidate = graph_text(arguments);
    const auto found = state->requests.find(candidate);
    if (found == state->requests.end()) {
        co_return std::unexpected(verification_error(
            errc::not_found, "graph query has no indexed action result", candidate));
    }
    auto cache_entry = co_await context.read(
        graph_input_key{graph_input_kind::environment, "action-result/" + candidate});
    if (!cache_entry) {
        co_return std::unexpected(cache_entry.error());
    }
    for (const auto& input : found->second.second.manifest) {
        auto read =
            co_await context.read(graph_input_key{graph_input_kind::working_tree_blob, input.path});
        if (!read) {
            co_return std::unexpected(read.error());
        }
    }
    auto lookup =
        co_await state->cache->lookup(found->second.second.input, found->second.second.sandbox);
    if (!lookup) {
        co_return std::unexpected(lookup.error());
    }
    if (!lookup->hit) {
        co_return std::vector<std::byte>{};
    }
    co_return graph_bytes(to_string(lookup->hit->value));
}

runtime::task<replay_report>
reexecute_one(repository& source, digest result, const action_result& recorded, bool explain) {
    auto action_bytes = co_await repository_detail::build_access::get(source, recorded.action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = objects::decode_action(*action_bytes);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto sandbox_bytes = co_await repository_detail::build_access::get(source, action->sandbox);
    if (!sandbox_bytes) {
        co_return std::unexpected(sandbox_bytes.error());
    }
    auto hydrated = co_await hydrate_replay_graph(source, recorded, *action);
    if (!hydrated) {
        co_return std::unexpected(hydrated.error());
    }
    auto expected_wrap_sandbox = objects::encode(wrap_sandbox_definition());
    if (!expected_wrap_sandbox) {
        co_return std::unexpected(expected_wrap_sandbox.error());
    }
    auto replayed = liquid::result<action_result>{};
    if (*sandbox_bytes == *expected_wrap_sandbox) {
        replayed = co_await run_wrapped_action(source, *action, recorded, recorded.observed_at);
    } else {
        replayed = co_await run_action(
            source, action->input, recorded.action, action->sandbox, recorded.observed_at);
    }
    if (!replayed) {
        co_return std::unexpected(replayed.error());
    }
    auto differences = co_await compare(source, recorded, *replayed, explain);
    if (!differences) {
        co_return std::unexpected(differences.error());
    }
    co_return replay_report{.action = recorded.action,
                            .result = result,
                            .recorded_exit = recorded.exit,
                            .reexecuted_exit = replayed->exit,
                            .agreement = differences->empty(),
                            .differences = std::move(*differences)};
}

auto difference_descriptions(const replay_report& replayed) -> std::vector<std::string> {
    auto differing_outputs = std::vector<std::string>{};
    for (const auto& difference : replayed.differences) {
        if (difference.output == "exit-status") {
            differing_outputs.push_back(
                "exit-status cached=" + std::to_string(replayed.recorded_exit) +
                " reexecuted=" + std::to_string(replayed.reexecuted_exit));
            continue;
        }
        differing_outputs.push_back(
            difference.output + " cached=" +
            (difference.cached ? to_string(*difference.cached) : "missing") + " reexecuted=" +
            (difference.reexecuted ? to_string(*difference.reexecuted) : "missing"));
    }
    return differing_outputs;
}

runtime::task<void> record_reexecution(repository& source,
                                       build_detail::disk_cache& cache,
                                       replay_report& replayed,
                                       digest sandbox) {
    auto signer = repository_detail::build_access::signer(source);
    if (!signer) {
        co_return std::unexpected(signer.error());
    }
    auto attestation = liquid::attestation{
        .subject = replayed.result,
        .profile = "reproducibility/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes = {{"verified_by", signer->taap_id},
                       {"sandbox", to_string(sandbox)},
                       {"agreement", replayed.agreement}}};
    auto differing_outputs = difference_descriptions(replayed);
    if (!differing_outputs.empty()) {
        attestation.attributes.push_back({"differing_outputs", differing_outputs});
    }
    auto signed_attestation =
        co_await repository_detail::build_access::sign_attestation(source, attestation);
    if (!signed_attestation) {
        co_return std::unexpected(signed_attestation.error());
    }
    auto encoded = objects::encode(attestation);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto attestation_id =
        co_await repository_detail::build_access::put_record(source, std::move(*encoded));
    if (!attestation_id) {
        co_return std::unexpected(attestation_id.error());
    }
    auto recorded =
        co_await cache.record_verification(replayed.result, *attestation_id, !replayed.agreement);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    replayed.attestation = *attestation_id;
    if (!replayed.agreement) {
        repository_detail::build_access::record_critical_alert(
            source, {replayed.action, replayed.result, replayed.differences});
    }
    co_return liquid::result<void>{};
}

runtime::task<verified_action> verify_one(repository& source,
                                          build_detail::disk_cache& cache,
                                          const objects::action_index_entry& entry,
                                          const action_result& recorded,
                                          bool explain) {
    auto action_bytes = co_await repository_detail::build_access::get(source, recorded.action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = objects::decode_action(*action_bytes);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto replayed = co_await reexecute_one(source, entry.result, recorded, explain);
    if (!replayed) {
        co_return std::unexpected(replayed.error());
    }
    auto recorded_reexecution =
        co_await record_reexecution(source, cache, *replayed, action->sandbox);
    if (!recorded_reexecution) {
        co_return std::unexpected(recorded_reexecution.error());
    }
    co_return verified_action{replayed->action,
                              replayed->result,
                              *replayed->attestation,
                              replayed->agreement,
                              std::move(replayed->differences)};
}

struct replay_target {
    digest result;
    action_result value;
};

runtime::task<replay_target>
resolve_replay_target(repository& source, build_detail::disk_cache& cache, digest target) {
    auto target_bytes = co_await repository_detail::build_access::get(source, target);
    if (!target_bytes) {
        co_return std::unexpected(target_bytes.error());
    }
    auto recorded = objects::decode_result(*target_bytes);
    if (recorded) {
        co_return replay_target{target, std::move(*recorded)};
    }
    auto action = objects::decode_action(*target_bytes);
    if (!action) {
        co_return std::unexpected(verification_error(
            errc::invalid_argument, "replay target is not an action or result", to_string(target)));
    }
    auto selected_result = std::optional<replay_target>{};
    auto selected_sequence = std::uint64_t{};
    for (const auto& entry : cache.entries()) {
        if (selected_result && entry.sequence <= selected_sequence) {
            continue;
        }
        auto result_bytes = co_await repository_detail::build_access::get(source, entry.result);
        if (!result_bytes) {
            if (result_bytes.error().code == errc::not_found) {
                continue;
            }
            co_return std::unexpected(result_bytes.error());
        }
        auto value = objects::decode_result(*result_bytes);
        if (!value || !same_digest(value->action, target)) {
            continue;
        }
        selected_sequence = entry.sequence;
        selected_result = replay_target{entry.result, std::move(*value)};
    }
    if (!selected_result) {
        co_return std::unexpected(verification_error(
            errc::not_found, "action is not present in the cache", to_string(target)));
    }
    co_return std::move(*selected_result);
}

} // namespace

runtime::task<verify_report> verify(repository& source, verify_options options) {
    if (options.selection == verify_selection::action && !options.action) {
        co_return std::unexpected(
            verification_error(errc::invalid_argument, "verification action is missing"));
    }
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto report = verify_report{};
    auto seen = std::set<std::string>{};
    const auto entries = cache.entries();
    const auto rate = repository_detail::build_access::verification_rate(source);
    for (const auto& entry : entries) {
        if (!seen.insert(to_string(entry.result)).second) {
            continue;
        }
        auto result_bytes = co_await repository_detail::build_access::get(source, entry.result);
        if (!result_bytes) {
            co_return std::unexpected(result_bytes.error());
        }
        auto cached = objects::decode_result(*result_bytes);
        if (!cached) {
            co_return std::unexpected(cached.error());
        }
        if (!selected(entry, *cached, options, rate)) {
            continue;
        }
        ++report.considered;
        auto verified = co_await verify_one(source, cache, entry, *cached, options.explain);
        if (!verified) {
            co_return std::unexpected(verified.error());
        }
        report.actions.push_back(std::move(*verified));
    }
    if (options.selection == verify_selection::action && report.considered == 0) {
        co_return std::unexpected(verification_error(
            errc::not_found, "action is not present in the cache", to_string(*options.action)));
    }
    co_return report;
}

runtime::task<verify_report> verify(std::filesystem::path working_tree, verify_options options) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree)) {
        co_return std::unexpected(verification_error(errc::invalid_argument,
                                                     "verify working tree is not a directory",
                                                     working_tree.string()));
    }
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto source = co_await repository::open(*cache_directory / "repository");
    if (!source) {
        co_return std::unexpected(source.error());
    }
    co_return co_await verify(*source, std::move(options));
}

runtime::task<graph_verify_report> verify_graph(std::filesystem::path working_tree,
                                                graph_verify_options options) {
    if (options.rate == 0) {
        co_return std::unexpected(
            verification_error(errc::invalid_argument, "graph verification rate must be positive"));
    }
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree)) {
        co_return std::unexpected(verification_error(errc::invalid_argument,
                                                     "verify working tree is not a directory",
                                                     working_tree.string()));
    }
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto source = co_await repository::open(*cache_directory / "repository");
    if (!source) {
        co_return std::unexpected(source.error());
    }
    auto cache = build_detail::disk_cache{*source};
    auto opened_cache = co_await cache.open();
    if (!opened_cache) {
        co_return std::unexpected(opened_cache.error());
    }
    auto state = graph_query_state{.cache = &cache};
    for (const auto& entry : cache.entries()) {
        if (entry.divergent) {
            continue;
        }
        const auto candidate = to_string(entry.candidate);
        const auto found = state.requests.find(candidate);
        if (found != state.requests.end() && entry.sequence <= found->second.first) {
            continue;
        }
        auto result_bytes = co_await repository_detail::build_access::get(*source, entry.result);
        if (!result_bytes) {
            co_return std::unexpected(result_bytes.error());
        }
        auto result = objects::decode_result(*result_bytes);
        if (!result) {
            co_return std::unexpected(result.error());
        }
        auto action_bytes = co_await repository_detail::build_access::get(*source, result->action);
        if (!action_bytes) {
            co_return std::unexpected(action_bytes.error());
        }
        auto action = objects::decode_action(*action_bytes);
        if (!action) {
            co_return std::unexpected(action.error());
        }
        state.requests.insert_or_assign(
            candidate,
            std::pair{entry.sequence,
                      graph_query_request{action->input, action->sandbox, result->manifest}});
    }
    auto registry = query_registry{};
    auto registered = registry.add(
        {.name = "action_result",
         .compute = [&state](query_context& context, std::vector<std::byte> arguments) {
             return clean_action_result_query(context, std::move(arguments), &state);
         }});
    if (!registered) {
        co_return std::unexpected(registered.error());
    }
    auto queries = co_await graph::open(
        *cache_directory / "query-graph", std::move(registry), {.working_tree = working_tree});
    if (!queries) {
        co_return std::unexpected(queries.error());
    }
    auto graph_report = co_await queries->validate({.rate = options.rate, .all = options.all});
    if (!graph_report) {
        co_return std::unexpected(graph_report.error());
    }
    auto action_report = co_await verify(
        *source,
        {.selection = options.all ? verify_selection::all : verify_selection::sample,
         .explain = options.explain});
    if (!action_report) {
        co_return std::unexpected(action_report.error());
    }
    co_return graph_verify_report{std::move(*graph_report), std::move(*action_report)};
}

runtime::task<replay_report> replay(repository& source, digest target, replay_options options) {
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto recorded = co_await resolve_replay_target(source, cache, target);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    auto replayed = co_await reexecute_one(source, recorded->result, recorded->value, true);
    if (!replayed) {
        co_return std::unexpected(replayed.error());
    }
    if (!options.record) {
        co_return replayed;
    }
    auto action_bytes = co_await repository_detail::build_access::get(source, replayed->action);
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action = objects::decode_action(*action_bytes);
    if (!action) {
        co_return std::unexpected(action.error());
    }
    auto recorded_reexecution =
        co_await record_reexecution(source, cache, *replayed, action->sandbox);
    if (!recorded_reexecution) {
        co_return std::unexpected(recorded_reexecution.error());
    }
    co_return replayed;
}

runtime::task<replay_report>
replay(std::filesystem::path working_tree, digest target, replay_options options) {
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree)) {
        co_return std::unexpected(verification_error(errc::invalid_argument,
                                                     "replay working tree is not a directory",
                                                     working_tree.string()));
    }
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : wrap_cache_directory(working_tree);
    if (!cache_directory) {
        co_return std::unexpected(cache_directory.error());
    }
    auto source = co_await repository::open(*cache_directory / "repository");
    if (!source) {
        co_return std::unexpected(source.error());
    }
    co_return co_await replay(*source, target, std::move(options));
}

} // namespace liquid::build

namespace liquid::build_detail {
namespace {

runtime::task<void> run_scheduled_graph_validation(std::filesystem::path working_tree,
                                                   std::filesystem::path cache_directory) {
    auto [yield_error] = co_await runtime::delay(std::chrono::nanoseconds{1});
    if (yield_error) {
        co_return std::unexpected(
            error{errc::internal, yield_error.message(), "scheduled graph validation"});
    }
    auto verified = co_await build::verify_graph(std::move(working_tree),
                                                 {.cache_directory = std::move(cache_directory)});
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    co_return result<void>{};
}

} // namespace

void schedule_graph_validation(std::filesystem::path working_tree,
                               std::filesystem::path cache_directory) {
    runtime::spawn(
        run_scheduled_graph_validation(std::move(working_tree), std::move(cache_directory)),
        [](result<void>) {});
}

} // namespace liquid::build_detail
