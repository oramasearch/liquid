#include "liquid/build.hpp"

#include <algorithm>
#include <array>
#include <limits>
#include <map>
#include <set>

#include "build/cache.hpp"
#include "build/executor.hpp"
#include "build/repository_access.hpp"
#include "objects/records.hpp"

namespace liquid {

namespace build {
namespace {

enum class graph_object_kind : std::uint8_t { tree, blob, bytes };

struct graph_objects {
    std::map<std::string, std::pair<digest, std::vector<std::byte>>> values;

    auto ids() const -> std::vector<digest> {
        auto output = std::vector<digest>{};
        output.reserve(values.size());
        for (const auto& [key, value] : values) {
            output.push_back(value.first);
        }
        return output;
    }

    auto blobs() const -> std::vector<remote_blob> {
        auto output = std::vector<remote_blob>{};
        output.reserve(values.size());
        for (const auto& [key, value] : values) {
            output.push_back({value.first, value.second});
        }
        return output;
    }

    auto bytes() const -> std::uint64_t {
        auto output = std::uint64_t{};
        for (const auto& [key, value] : values) {
            output += value.second.size();
        }
        return output;
    }
};

struct batch_action {
    std::size_t index{};
    prepared_action prepared;
    execution_schedule schedule;
    graph_objects graph;
};

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

runtime::task<void>
collect_graph(repository& source, digest id, graph_object_kind kind, graph_objects& output) {
    const auto key = to_string(id);
    if (output.values.contains(key)) {
        co_return liquid::result<void>{};
    }
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    output.values.emplace(key, std::pair{id, *bytes});
    if (kind == graph_object_kind::tree) {
        auto tree = objects::decode_tree(*bytes);
        if (!tree) {
            co_return std::unexpected(tree.error());
        }
        for (const auto& entry : tree->entries) {
            if (entry.kind == node_kind::tree) {
                auto nested =
                    co_await collect_graph(source, entry.ref, graph_object_kind::tree, output);
                if (!nested) {
                    co_return std::unexpected(nested.error());
                }
            } else if (entry.kind == node_kind::blob) {
                auto nested =
                    co_await collect_graph(source, entry.ref, graph_object_kind::blob, output);
                if (!nested) {
                    co_return std::unexpected(nested.error());
                }
            }
        }
    } else if (kind == graph_object_kind::blob) {
        auto blob = objects::decode_blob(*bytes);
        if (!blob) {
            co_return std::unexpected(blob.error());
        }
        for (const auto& chunk : blob->chunks) {
            auto nested = co_await collect_graph(source, chunk, graph_object_kind::bytes, output);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<std::vector<std::byte>>
read_remote(remote_blob_source& source, const std::string& instance, digest id) {
    auto output = std::vector<std::byte>{};
    // On the heap: a 64 KiB array would make the Debug coroutine ramp reserve that much stack.
    auto buffer = std::vector<std::byte>(64U * 1024U);
    for (;;) {
        auto count = co_await source.read_range(instance, id, output.size(), buffer);
        if (!count) {
            co_return std::unexpected(count.error());
        }
        if (*count > buffer.size()) {
            co_return std::unexpected(
                error{errc::corrupt, "remote read exceeded the requested range", to_string(id)});
        }
        output.insert(output.end(), buffer.begin(), buffer.begin() + *count);
        if (*count < buffer.size()) {
            break;
        }
    }
    co_return output;
}

runtime::task<void> hydrate_object(repository& destination,
                                   remote_blob_source& source,
                                   const std::string& instance,
                                   digest id,
                                   graph_object_kind kind,
                                   std::set<std::string>& seen) {
    if (!seen.insert(to_string(id)).second) {
        co_return liquid::result<void>{};
    }
    auto bytes = co_await read_remote(source, instance, id);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto stored = co_await repository_detail::build_access::put_record(
        destination, std::move(*bytes), id.alg);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (!same_digest(*stored, id)) {
        co_return std::unexpected(
            error{errc::corrupt, "remote object digest does not match", to_string(id)});
    }
    if (kind != graph_object_kind::blob) {
        co_return liquid::result<void>{};
    }
    auto record = co_await repository_detail::build_access::get(destination, id);
    if (!record) {
        co_return std::unexpected(record.error());
    }
    auto blob = objects::decode_blob(*record);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    for (const auto& chunk : blob->chunks) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, chunk, graph_object_kind::bytes, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<void> hydrate_result(repository& destination,
                                   remote_blob_source& source,
                                   const std::string& instance,
                                   const action_result& value) {
    auto seen = std::set<std::string>{};
    for (const auto& id : {value.stdout_output, value.stderr_output}) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, id, graph_object_kind::blob, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    for (const auto& output : value.outputs) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, output.blob, graph_object_kind::blob, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    if (value.provenance) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, *value.provenance, graph_object_kind::bytes, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    for (const auto& id : value.sboms) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, id, graph_object_kind::bytes, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
        auto bytes = co_await repository_detail::build_access::get(destination, id);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto statement = objects::decode_attestation(*bytes);
        if (!statement) {
            co_return std::unexpected(statement.error());
        }
        const auto document = std::ranges::find(
            statement->attributes, std::string{"document"}, &attestation_attribute::name);
        if (document == statement->attributes.end()) {
            co_return std::unexpected(
                error{errc::corrupt, "remote SBOM has no document", to_string(id)});
        }
        const auto* text = std::get_if<std::string>(&document->value);
        auto document_id =
            text ? parse_digest(*text)
                 : liquid::result<digest>{std::unexpected(
                       error{errc::corrupt, "remote SBOM document is invalid", to_string(id)})};
        if (!document_id) {
            co_return std::unexpected(document_id.error());
        }
        hydrated = co_await hydrate_object(
            destination, source, instance, *document_id, graph_object_kind::bytes, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    for (const auto& id : value.abis) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, id, graph_object_kind::bytes, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    for (const auto& id : value.abi_compatibility) {
        auto hydrated = co_await hydrate_object(
            destination, source, instance, id, graph_object_kind::bytes, seen);
        if (!hydrated) {
            co_return std::unexpected(hydrated.error());
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<void> upload_missing(remote_blob_source& destination,
                                   const std::string& instance,
                                   const graph_objects& graph,
                                   std::span<const digest> missing) {
    for (const auto& id : missing) {
        const auto found = graph.values.find(to_string(id));
        if (found == graph.values.end()) {
            co_return std::unexpected(
                error{errc::corrupt, "remote requested an unknown input object", to_string(id)});
        }
        auto written = co_await destination.write(instance, id, found->second.second);
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<std::vector<action_timing>> history(repository& source, digest candidate) {
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    auto entries = cache.entries();
    std::ranges::sort(entries, std::greater{}, &objects::action_index_entry::sequence);
    auto output = std::vector<action_timing>{};
    auto seen = std::set<std::string>{};
    for (const auto& entry : entries) {
        if (!same_digest(entry.candidate, candidate) || entry.divergent ||
            !seen.insert(to_string(entry.result)).second) {
            continue;
        }
        auto bytes = co_await repository_detail::build_access::get(source, entry.result);
        if (!bytes) {
            continue;
        }
        auto value = objects::decode_result(*bytes);
        if (value) {
            output.push_back(value->timing);
        }
        if (output.size() == 8) {
            break;
        }
    }
    co_return output;
}

boost::capy::io_task<action_result> run_local_candidate(repository& source,
                                                        const prepared_action& prepared,
                                                        std::optional<error>& failure) {
    auto value = co_await run_action(
        source, prepared.input, prepared.action, prepared.sandbox, prepared.observed_at);
    if (!value) {
        failure = value.error();
        co_return boost::capy::io_result<action_result>{
            std::make_error_code(std::errc::operation_canceled), action_result{}};
    }
    co_return boost::capy::io_result<action_result>{{}, std::move(*value)};
}

runtime::task<action_result> run_remote_candidate(repository& source,
                                                  const prepared_action& prepared,
                                                  const execution_schedule& schedule,
                                                  remote_executor& remote) {
    auto value = co_await remote.execute_prepared(
        prepared.input, prepared.action, prepared.sandbox, schedule);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    value->action = prepared.action;
    auto hydrated = co_await hydrate_result(source, remote.blobs(), remote.instance(), *value);
    if (!hydrated) {
        co_return std::unexpected(hydrated.error());
    }
    co_return std::move(*value);
}

boost::capy::io_task<action_result> run_remote_race_candidate(repository& source,
                                                              const prepared_action& prepared,
                                                              const execution_schedule& schedule,
                                                              remote_executor& remote,
                                                              std::optional<error>& failure) {
    auto value = co_await run_remote_candidate(source, prepared, schedule, remote);
    if (!value) {
        failure = value.error();
        co_return boost::capy::io_result<action_result>{
            std::make_error_code(std::errc::operation_canceled), action_result{}};
    }
    co_return boost::capy::io_result<action_result>{{}, std::move(*value)};
}

auto duration(const action_timing& value) -> std::chrono::milliseconds {
    return value.queued + value.sandbox + value.exec + value.hash + value.store;
}

auto classify(std::chrono::milliseconds median) -> action_size_class {
    if (median <= std::chrono::milliseconds{25}) {
        return action_size_class::tiny;
    }
    if (median <= std::chrono::milliseconds{250}) {
        return action_size_class::short_action;
    }
    if (median >= std::chrono::seconds{2}) {
        return action_size_class::long_action;
    }
    return action_size_class::medium;
}

auto saturated_milliseconds(std::uint64_t value) -> std::chrono::milliseconds {
    constexpr auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
    return std::chrono::milliseconds{static_cast<std::int64_t>(std::min(value, maximum))};
}

} // namespace

auto choose_execution(scheduling_inputs inputs) -> execution_schedule {
    auto samples = std::vector<std::chrono::milliseconds>{};
    samples.reserve(inputs.history.size());
    for (const auto& value : inputs.history) {
        samples.push_back(duration(value));
    }
    std::ranges::sort(samples);
    if (samples.empty()) {
        inputs.size_class = action_size_class::unknown;
        return {.decision = execution_decision::local,
                .inputs = std::move(inputs),
                .reason = "no prior timing; measure locally before remote dispatch"};
    }

    const auto median = samples[samples.size() / 2];
    inputs.size_class = classify(median);
    const auto queue_ms = inputs.remote_queue_depth * 50U;
    const auto transfer_ms = (inputs.remote_missing_bytes * 1000U) / (100U * 1024U * 1024U);
    const auto remote_exec_ms =
        static_cast<std::uint64_t>(std::max<std::int64_t>(1, median.count())) * 3U / 4U;
    const auto remote_estimate =
        saturated_milliseconds(20U + queue_ms + transfer_ms + remote_exec_ms);
    const auto local_estimate = median;

    if (inputs.size_class == action_size_class::tiny) {
        return {.decision = execution_decision::local,
                .inputs = std::move(inputs),
                .local_estimate = local_estimate,
                .remote_estimate = remote_estimate,
                .reason = "tiny actions stay local"};
    }
    if (inputs.size_class == action_size_class::short_action) {
        const auto batched = inputs.batch_supported && inputs.batch_size >= 4;
        return {.decision = batched ? execution_decision::remote : execution_decision::local,
                .inputs = std::move(inputs),
                .local_estimate = local_estimate,
                .remote_estimate = remote_estimate,
                .reason = batched ? "short actions share one remote batch"
                                  : "short action dispatch costs more than local execution"};
    }

    const auto low = samples.front();
    const auto high = samples.back();
    const auto uncertain = samples.size() >= 3 && high - low > median / 2;
    const auto remote_congested = inputs.remote_queue_depth > 8;
    const auto remote_cold = inputs.remote_state_queried &&
                             inputs.remote_cache_locality_permille < 250 &&
                             inputs.remote_missing_bytes > 64U * 1024U * 1024U;
    if (uncertain && !remote_congested && !remote_cold) {
        return {.decision = execution_decision::race,
                .inputs = std::move(inputs),
                .local_estimate = local_estimate,
                .remote_estimate = remote_estimate,
                .reason = "recorded duration varies; race local and remote"};
    }
    const auto decision =
        remote_estimate < local_estimate ? execution_decision::remote : execution_decision::local;
    return {.decision = decision,
            .inputs = std::move(inputs),
            .local_estimate = local_estimate,
            .remote_estimate = remote_estimate,
            .reason = decision == execution_decision::remote
                          ? "remote queue, transfer, and execution estimate is lower"
                          : "local execution estimate is lower"};
}

runtime::task<result_id> execute_selected(repository& source,
                                          action_input input,
                                          remote_executor& remote,
                                          std::optional<digest> history_candidate) {
    auto prepared = co_await prepare_action(source, std::move(input));
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (prepared->hit) {
        repository_detail::build_access::record_cache_lookup(
            source, prepared->hit->value, cache_status::hit, {}, prepared->bytes_served);
        co_return *prepared->hit;
    }

    auto recorded = co_await history(source, history_candidate.value_or(prepared->candidate));
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    auto inputs = scheduling_inputs{.history = std::move(*recorded),
                                    .remote_queue_depth = remote.queue_depth(),
                                    .batch_supported = remote.supports_batch()};
    auto selected = choose_execution(inputs);
    auto graph = graph_objects{};
    auto missing = std::vector<digest>{};
    if (selected.inputs.size_class != action_size_class::unknown &&
        selected.inputs.size_class != action_size_class::tiny &&
        selected.inputs.size_class != action_size_class::short_action) {
        auto source_graph =
            co_await collect_graph(source, prepared->input.inputs, graph_object_kind::tree, graph);
        if (!source_graph) {
            co_return std::unexpected(source_graph.error());
        }
        auto toolchain_graph = co_await collect_graph(
            source, prepared->input.toolchain, graph_object_kind::tree, graph);
        if (!toolchain_graph) {
            co_return std::unexpected(toolchain_graph.error());
        }
        auto ids = graph.ids();
        auto prepared = co_await remote.blobs().prepare(remote.instance(), graph.blobs());
        if (!prepared) {
            co_return std::unexpected(prepared.error());
        }
        auto remote_missing =
            co_await remote.blobs().find_missing(remote.instance(), std::move(ids));
        if (!remote_missing) {
            co_return std::unexpected(remote_missing.error());
        }
        missing = std::move(*remote_missing);
        auto missing_bytes = std::uint64_t{};
        for (const auto& id : missing) {
            const auto found = graph.values.find(to_string(id));
            if (found == graph.values.end()) {
                co_return std::unexpected(error{
                    errc::corrupt, "remote reported an unknown missing object", to_string(id)});
            }
            missing_bytes += found->second.second.size();
        }
        inputs.input_bytes = graph.bytes();
        inputs.remote_state_queried = true;
        inputs.remote_missing_bytes = missing_bytes;
        inputs.remote_cache_locality_permille =
            inputs.input_bytes == 0
                ? 1000
                : static_cast<std::uint32_t>((inputs.input_bytes - missing_bytes) * 1000U /
                                             inputs.input_bytes);
        selected = choose_execution(std::move(inputs));
    }

    auto value = liquid::result<action_result>{std::unexpected(
        error{errc::internal, "execution scheduler selected no branch", "build::execute"})};
    if (selected.decision == execution_decision::local) {
        value = co_await run_action(
            source, prepared->input, prepared->action, prepared->sandbox, prepared->observed_at);
    } else {
        auto uploaded = co_await upload_missing(remote.blobs(), remote.instance(), graph, missing);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
        if (selected.decision == execution_decision::remote) {
            value = co_await run_remote_candidate(source, *prepared, selected, remote);
        } else {
            auto local_failure = std::optional<error>{};
            auto remote_failure = std::optional<error>{};
            auto winner = co_await boost::capy::when_any(
                run_local_candidate(source, *prepared, local_failure),
                run_remote_race_candidate(source, *prepared, selected, remote, remote_failure));
            if (winner.index() == 1) {
                value = std::move(std::get<1>(winner));
            } else if (winner.index() == 2) {
                value = std::move(std::get<2>(winner));
            } else if (local_failure) {
                value = std::unexpected(*local_failure);
            } else if (remote_failure) {
                value = std::unexpected(*remote_failure);
            } else {
                value = std::unexpected(
                    error{errc::unavailable, "local and remote execution failed", "race"});
            }
        }
    }
    if (!value) {
        co_return std::unexpected(value.error());
    }
    if (!value->schedule) {
        value->schedule = std::move(selected);
    }
    value->record_version = 4;
    co_return co_await finish_action(source, *prepared, std::move(*value));
}

runtime::task<result_id> execute(repository& source, action_input input, remote_executor& remote) {
    co_return co_await execute_selected(source, std::move(input), remote, std::nullopt);
}

runtime::task<result_id> execute_with_history_candidate(repository& source,
                                                        action_input input,
                                                        remote_executor& remote,
                                                        digest history_candidate) {
    co_return co_await execute_selected(
        source, std::move(input), remote, std::move(history_candidate));
}

runtime::task<std::vector<result_id>>
execute_batch(repository& source, std::vector<action_input> inputs, remote_executor& remote) {
    auto output = std::vector<std::optional<result_id>>(inputs.size());
    auto pending = std::vector<batch_action>{};
    pending.reserve(inputs.size());
    auto short_count = std::uint64_t{};
    for (auto index = std::size_t{}; index != inputs.size(); ++index) {
        auto prepared = co_await prepare_action(source, inputs[index]);
        if (!prepared) {
            co_return std::unexpected(prepared.error());
        }
        if (prepared->hit) {
            repository_detail::build_access::record_cache_lookup(
                source, prepared->hit->value, cache_status::hit, {}, prepared->bytes_served);
            output[index] = *prepared->hit;
            continue;
        }
        auto recorded = co_await history(source, prepared->candidate);
        if (!recorded) {
            co_return std::unexpected(recorded.error());
        }
        auto schedule = choose_execution({.history = std::move(*recorded),
                                          .remote_queue_depth = remote.queue_depth(),
                                          .batch_supported = remote.supports_batch()});
        if (schedule.inputs.size_class == action_size_class::short_action) {
            ++short_count;
        }
        pending.push_back(
            {.index = index, .prepared = std::move(*prepared), .schedule = std::move(schedule)});
    }

    const auto can_batch = remote.supports_batch() && short_count >= 4;
    auto batched = std::vector<std::size_t>{};
    if (can_batch) {
        for (auto index = std::size_t{}; index != pending.size(); ++index) {
            auto& action = pending[index];
            if (action.schedule.inputs.size_class != action_size_class::short_action) {
                continue;
            }
            action.schedule.inputs.batch_size = short_count;
            action.schedule = choose_execution(std::move(action.schedule.inputs));
            batched.push_back(index);
        }
    }

    if (!batched.empty()) {
        auto combined = graph_objects{};
        for (const auto index : batched) {
            auto& action = pending[index];
            auto source_graph = co_await collect_graph(
                source, action.prepared.input.inputs, graph_object_kind::tree, action.graph);
            if (!source_graph) {
                co_return std::unexpected(source_graph.error());
            }
            auto toolchain_graph = co_await collect_graph(
                source, action.prepared.input.toolchain, graph_object_kind::tree, action.graph);
            if (!toolchain_graph) {
                co_return std::unexpected(toolchain_graph.error());
            }
            combined.values.insert(action.graph.values.begin(), action.graph.values.end());
        }
        auto combined_ids = combined.ids();
        auto prepared = co_await remote.blobs().prepare(remote.instance(), combined.blobs());
        if (!prepared) {
            co_return std::unexpected(prepared.error());
        }
        auto missing =
            co_await remote.blobs().find_missing(remote.instance(), std::move(combined_ids));
        if (!missing) {
            co_return std::unexpected(missing.error());
        }
        for (const auto index : batched) {
            auto& action = pending[index];
            auto missing_bytes = std::uint64_t{};
            for (const auto& id : *missing) {
                const auto found = action.graph.values.find(to_string(id));
                if (found != action.graph.values.end()) {
                    missing_bytes += found->second.second.size();
                }
            }
            auto model = std::move(action.schedule.inputs);
            model.input_bytes = action.graph.bytes();
            model.remote_state_queried = true;
            model.remote_missing_bytes = missing_bytes;
            model.remote_cache_locality_permille =
                model.input_bytes == 0
                    ? 1000
                    : static_cast<std::uint32_t>((model.input_bytes - missing_bytes) * 1000U /
                                                 model.input_bytes);
            action.schedule = choose_execution(std::move(model));
        }
        auto uploaded =
            co_await upload_missing(remote.blobs(), remote.instance(), combined, *missing);
        if (!uploaded) {
            co_return std::unexpected(uploaded.error());
        }
        auto remote_inputs = std::vector<action_input>{};
        remote_inputs.reserve(batched.size());
        for (const auto index : batched) {
            remote_inputs.push_back(pending[index].prepared.input);
        }
        auto values = co_await remote.execute_batch(std::move(remote_inputs));
        if (!values) {
            co_return std::unexpected(values.error());
        }
        if (values->size() != batched.size()) {
            co_return std::unexpected(error{errc::corrupt,
                                            "remote batch result count does not match request",
                                            std::to_string(values->size())});
        }
        for (auto index = std::size_t{}; index != batched.size(); ++index) {
            auto& action = pending[batched[index]];
            auto& value = (*values)[index];
            value.action = action.prepared.action;
            auto hydrated =
                co_await hydrate_result(source, remote.blobs(), remote.instance(), value);
            if (!hydrated) {
                co_return std::unexpected(hydrated.error());
            }
            value.schedule = action.schedule;
            value.record_version = 4;
        }
        for (auto index = std::size_t{}; index != batched.size(); ++index) {
            auto& action = pending[batched[index]];
            auto finished =
                co_await finish_action(source, action.prepared, std::move((*values)[index]));
            if (!finished) {
                co_return std::unexpected(finished.error());
            }
            output[action.index] = *finished;
        }
    }

    for (auto index = std::size_t{}; index != pending.size(); ++index) {
        if (std::ranges::find(batched, index) != batched.end()) {
            continue;
        }
        auto value = co_await execute(source, pending[index].prepared.input, remote);
        if (!value) {
            co_return std::unexpected(value.error());
        }
        output[pending[index].index] = *value;
    }

    auto results = std::vector<result_id>{};
    results.reserve(output.size());
    for (auto& value : output) {
        if (!value) {
            co_return std::unexpected(
                error{errc::internal, "batch action produced no result", "build::execute_batch"});
        }
        results.push_back(*value);
    }
    co_return results;
}

} // namespace build
} // namespace liquid
