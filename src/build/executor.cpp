#include "liquid/build.hpp"

#include <algorithm>
#include <atomic>
#include <fstream>
#include <mutex>
#include <set>
#include <sys/stat.h>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "build/cache.hpp"
#include "build/executor.hpp"
#include "build/materialise.hpp"
#include "build/policy.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "build/why.hpp"
#include "liquid/graph.hpp"
#include "objects/records.hpp"
#include "sandbox/environment.hpp"
#include "sandbox/sandbox.hpp"
#include "vfs/backend.hpp"

namespace liquid::build {
namespace {

struct observations {
    std::mutex mutex;
    std::vector<vfs_detail::observed_request> requests;
};

struct run_paths {
    std::filesystem::path run;
    std::filesystem::path mount;
    std::filesystem::path root;
    std::filesystem::path toolchain;
    std::filesystem::path outputs;
    std::filesystem::path stdout_path;
    std::filesystem::path stderr_path;
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto host_platform() -> platform {
#if defined(__linux__)
    constexpr auto os = "linux";
#elif defined(__APPLE__)
    constexpr auto os = "macos";
#elif defined(_WIN32)
    constexpr auto os = "windows";
#else
    constexpr auto os = "unknown";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr auto arch = "x86_64";
#elif defined(__aarch64__) || defined(__arm64__) || defined(_M_ARM64)
    constexpr auto arch = "arm64";
#else
    constexpr auto arch = "unknown";
#endif
    return {os, arch};
}

auto valid_relative_path(std::string_view value, bool allow_empty) -> bool {
    if (value.empty())
        return allow_empty;
    const auto path = std::filesystem::path{value};
    if (path.is_absolute())
        return false;
    for (const auto& component : path) {
        if (component == "..")
            return false;
    }
    return path.lexically_normal() == path;
}

auto validate(const action_input& input, bool require_host_platform = true)
    -> liquid::result<void> {
    if (input.command.empty() || input.command.front().empty())
        return std::unexpected(failure(errc::invalid_argument, "action command is empty"));
    const auto host = host_platform();
    if (require_host_platform &&
        (input.platform.os != host.os || input.platform.arch != host.arch)) {
        return std::unexpected(failure(errc::unsupported,
                                       "action platform does not match this worker",
                                       input.platform.os + "/" + input.platform.arch));
    }
    if (!valid_relative_path(input.cwd, true))
        return std::unexpected(failure(
            errc::invalid_argument, "action cwd must be a normalized relative path", input.cwd));
    auto seen = std::set<std::string>{};
    for (const auto& output : input.outputs) {
        const auto path = output.ends_with('/') ? output.substr(0, output.size() - 1) : output;
        if (!valid_relative_path(path, false))
            return std::unexpected(failure(errc::invalid_argument,
                                           "declared output must be a normalized relative path",
                                           output));
        if (!seen.insert(output).second)
            return std::unexpected(
                failure(errc::invalid_argument, "declared output is duplicated", output));
    }
    if (input.timeout && input.timeout->count() <= 0)
        return std::unexpected(failure(errc::invalid_argument, "action timeout must be positive"));
    return {};
}

auto prepare_run(const std::filesystem::path& repository_root, const digest& action)
    -> liquid::result<run_paths> {
    static std::atomic_uint64_t sequence{};
    auto token = to_string(action);
    std::ranges::replace(token, ':', '-');
#if defined(_WIN32)
    const auto process = std::uint64_t{};
#else
    const auto process = static_cast<std::uint64_t>(::getpid());
#endif
    const auto run =
        repository_root / ".liquid-build" / "runs" /
        (token + "-" + std::to_string(process) + "-" + std::to_string(sequence.fetch_add(1)));
    run_paths output{.run = run,
                     .mount = run / "mount",
                     .root = run / "root",
                     .toolchain = run / "toolchain",
                     .outputs = run / "outputs",
                     .stdout_path = run / "stdout",
                     .stderr_path = run / "stderr"};
    auto code = std::error_code{};
    for (const auto& path : {output.mount, output.root, output.toolchain, output.outputs}) {
        std::filesystem::create_directories(path, code);
        if (code)
            return std::unexpected(failure(errc::io, code.message(), path.string()));
    }
    return output;
}

auto remove_run(const run_paths& paths) -> liquid::result<void> {
    if (paths.run.filename().empty() || paths.run.parent_path().filename() != "runs")
        return std::unexpected(failure(
            errc::internal, "refusing to remove an invalid sandbox path", paths.run.string()));
    auto code = std::error_code{};
    std::filesystem::remove_all(paths.run, code);
    if (code)
        return std::unexpected(failure(errc::io, code.message(), paths.run.string()));
    return {};
}

auto executable_command(const action_input& input,
                        const sandbox_detail::environment_result& environment)
    -> std::vector<std::string> {
    auto command = input.command;
    if (std::filesystem::path{command.front()}.is_relative() &&
        command.front().find('/') == std::string::npos) {
        command.front() = environment.toolchain_root + "/bin/" + command.front();
    }
    return command;
}

auto manifest(const std::shared_ptr<observations>& observed)
    -> liquid::result<std::vector<observed_input>> {
    auto entries = std::map<std::string, digest>{};
    auto lock = std::scoped_lock{observed->mutex};
    for (const auto& request : observed->requests) {
        if ((request.operation != vfs_detail::observed_operation::open &&
             request.operation != vfs_detail::observed_operation::read) ||
            request.path.empty() || request.denied || !request.content)
            continue;
        const auto found = entries.find(request.path);
        if (found != entries.end() && to_string(found->second) != to_string(*request.content))
            return std::unexpected(
                failure(errc::internal, "observed path changed during an action", request.path));
        entries[request.path] = *request.content;
    }
    auto output = std::vector<observed_input>{};
    output.reserve(entries.size());
    for (const auto& [path, content] : entries)
        output.push_back({path, content});
    return output;
}

auto denied_write(const std::shared_ptr<observations>& observed) -> std::optional<std::string> {
    auto lock = std::scoped_lock{observed->mutex};
    for (const auto& request : observed->requests) {
        if (request.denied && request.operation == vfs_detail::observed_operation::write)
            return request.path;
    }
    return std::nullopt;
}

runtime::task<std::vector<action_output>>
collect_outputs(repository& source, const action_input& input, const run_paths& paths) {
    auto output = std::vector<action_output>{};
    for (const auto& declared : input.outputs) {
        const auto directory = declared.ends_with('/');
        const auto relative = directory ? declared.substr(0, declared.size() - 1) : declared;
        const auto path = paths.outputs / relative;
        auto code = std::error_code{};
        if (!std::filesystem::exists(path, code))
            continue;
        if (code)
            co_return std::unexpected(failure(errc::io, code.message(), path.string()));
        auto files = std::vector<std::filesystem::path>{};
        if (directory) {
            for (auto iterator = std::filesystem::recursive_directory_iterator{path, code};
                 !code && iterator != std::filesystem::recursive_directory_iterator{};
                 ++iterator) {
                if (iterator->is_regular_file())
                    files.push_back(iterator->path());
            }
            if (code)
                co_return std::unexpected(failure(errc::io, code.message(), path.string()));
        } else if (std::filesystem::is_regular_file(path, code)) {
            files.push_back(path);
        }
        for (const auto& file : files) {
            auto stored = co_await repository_detail::build_access::put_file(source, file);
            if (!stored)
                co_return std::unexpected(stored.error());
            const auto status = std::filesystem::status(file, code);
            if (code)
                co_return std::unexpected(failure(errc::io, code.message(), file.string()));
            const auto mode = static_cast<std::uint32_t>(status.permissions()) & 0777U;
            output.push_back(
                {std::filesystem::relative(file, paths.outputs).generic_string(), mode, *stored});
        }
    }
    std::ranges::sort(output, {}, &action_output::path);
    co_return output;
}

} // namespace

runtime::task<action_result> run_action(repository& source,
                                        action_input input,
                                        digest action,
                                        digest sandbox,
                                        std::chrono::sys_seconds observed_at) {
    const auto stop = co_await boost::capy::this_coro::stop_token;
    if (stop.stop_requested()) {
        co_return std::unexpected(failure(errc::unavailable, "action execution was cancelled"));
    }
    const auto sandbox_started = std::chrono::steady_clock::now();
    const auto wall_started =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto valid = validate(input);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto stored_sandbox = co_await repository_detail::build_access::get(source, sandbox);
    if (!stored_sandbox) {
        co_return std::unexpected(stored_sandbox.error());
    }
    auto current_sandbox = sandbox_detail::definition(input.platform);
    if (!current_sandbox) {
        co_return std::unexpected(current_sandbox.error());
    }
    auto current_sandbox_bytes = objects::encode(*current_sandbox);
    if (!current_sandbox_bytes) {
        co_return std::unexpected(current_sandbox_bytes.error());
    }
    if (*stored_sandbox != *current_sandbox_bytes) {
        co_return std::unexpected(failure(
            errc::unsupported, "recorded sandbox definition is unavailable", to_string(sandbox)));
    }
    auto root = repository_detail::build_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto prepared =
        co_await runtime::offload([root = *root, action] { return prepare_run(root, action); });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        co_return std::unexpected(prepared->error());
    }
    auto paths = std::move(**prepared);
    auto toolchain =
        co_await build_detail::materialise_tree(source, input.toolchain, paths.toolchain);
    if (!toolchain) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(toolchain.error());
    }
    auto observed = std::make_shared<observations>();
    auto mounted = co_await vfs_detail::start_observed_fuse(
        source,
        input.inputs,
        paths.mount,
        input.outputs,
        [observed](vfs_detail::observed_request request) {
            auto lock = std::scoped_lock{observed->mutex};
            observed->requests.push_back(std::move(request));
        });
    if (!mounted) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(mounted.error());
    }
    auto environment = sandbox_detail::environment(input, action, observed_at);
    if (!environment) {
        static_cast<void>(co_await (*mounted)->unmount());
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(environment.error());
    }
    const auto virtual_cwd =
        environment->work_root + (input.cwd.empty() ? std::string{} : "/" + input.cwd);
    sandbox_detail::launch_request launch_request{.sandbox_root = paths.root,
                                                  .input_mount = paths.mount,
                                                  .toolchain = paths.toolchain,
                                                  .output_root = paths.outputs,
                                                  .stdout_path = paths.stdout_path,
                                                  .stderr_path = paths.stderr_path,
                                                  .work_root = environment->work_root,
                                                  .toolchain_root = environment->toolchain_root,
                                                  .home_root = environment->home_root,
                                                  .temporary_root = environment->temporary_root,
                                                  .cwd = virtual_cwd,
                                                  .command =
                                                      executable_command(input, *environment),
                                                  .environment = environment->values,
                                                  .outputs = input.outputs,
                                                  .timeout = input.timeout,
                                                  .remote_eligible = input.remote_eligible,
                                                  .stop = stop};
    build_detail::testing::record_sandbox_spawn();
    const auto execution_started = std::chrono::steady_clock::now();
    auto launched = co_await runtime::offload(
        [launch_request] { return sandbox_detail::launch(launch_request); });
    const auto execution_finished = std::chrono::steady_clock::now();
    const auto hash_started = execution_finished;
    auto unmounted = co_await (*mounted)->unmount();
    if (!unmounted) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(unmounted.error());
    }
    if (!launched) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(launched.error());
    }
    if (!*launched) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(launched->error());
    }
    if ((*launched)->timed_out) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(failure(errc::unavailable, "action timed out"));
    }
    if ((*launched)->forbidden_read) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(failure(errc::unauthorized,
                                          "remote-eligible action read outside its inputs",
                                          *(*launched)->forbidden_read));
    }
    if ((*launched)->cancelled) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(failure(errc::unavailable, "action execution was cancelled"));
    }
    if (auto violation = denied_write(observed)) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(
            failure(errc::unauthorized, "action wrote outside declared outputs", *violation));
    }
    auto read_manifest = manifest(observed);
    const auto hash_finished = std::chrono::steady_clock::now();
    const auto store_started = hash_finished;
    auto stdout_id = co_await repository_detail::build_access::put_file(source, paths.stdout_path);
    auto stderr_id = co_await repository_detail::build_access::put_file(source, paths.stderr_path);
    auto outputs = co_await collect_outputs(source, input, paths);
    if (!stdout_id || !stderr_id || !outputs || !read_manifest) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        if (!stdout_id) {
            co_return std::unexpected(stdout_id.error());
        }
        if (!stderr_id) {
            co_return std::unexpected(stderr_id.error());
        }
        if (!outputs) {
            co_return std::unexpected(outputs.error());
        }
        co_return std::unexpected(read_manifest.error());
    }
    const auto store_finished = std::chrono::steady_clock::now();
    const auto wall_finished =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    action_result output{
        .action = action,
        .exit = (*launched)->exit,
        .outputs = std::move(*outputs),
        .stdout_output = *stdout_id,
        .stderr_output = *stderr_id,
        .manifest = std::move(*read_manifest),
        .timing = {.sandbox = std::chrono::duration_cast<std::chrono::milliseconds>(
                       execution_started - sandbox_started),
                   .exec = std::chrono::duration_cast<std::chrono::milliseconds>(
                       execution_finished - execution_started),
                   .hash = std::chrono::duration_cast<std::chrono::milliseconds>(hash_finished -
                                                                                 hash_started),
                   .store = std::chrono::duration_cast<std::chrono::milliseconds>(store_finished -
                                                                                  store_started)},
        .worker = input.platform.os + "/" + input.platform.arch,
        .observed_at = observed_at};
    output.started_at = wall_started;
    output.finished_at = wall_finished;
    auto removed = co_await runtime::offload([paths] { return remove_run(paths); });
    if (!removed) {
        co_return std::unexpected(removed.error());
    }
    if (!*removed) {
        co_return std::unexpected(removed->error());
    }
    co_return output;
}

runtime::task<prepared_action>
prepare_action(repository& source, action_input input, bool lookup_cache) {
    auto valid = validate(input);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    auto policy = co_await build_detail::enforce_policy(source, input);
    if (!policy) {
        co_return std::unexpected(policy.error());
    }
    if (!policy->allowed) {
        co_return std::unexpected(build_detail::policy_refusal(*policy));
    }
    auto source_time = co_await repository_detail::build_access::change_time(source, input.inputs);
    if (!source_time) {
        co_return std::unexpected(source_time.error());
    }
    auto sandbox = sandbox_detail::definition(input.platform);
    if (!sandbox) {
        co_return std::unexpected(sandbox.error());
    }
    auto sandbox_bytes = objects::encode(*sandbox);
    if (!sandbox_bytes) {
        co_return std::unexpected(sandbox_bytes.error());
    }
    auto sandbox_id =
        co_await repository_detail::build_access::put_record(source, std::move(*sandbox_bytes));
    if (!sandbox_id) {
        co_return std::unexpected(sandbox_id.error());
    }
    auto action_bytes = objects::encode(objects::action_record{input, *sandbox_id});
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action_id =
        co_await repository_detail::build_access::put_record(source, std::move(*action_bytes));
    if (!action_id) {
        co_return std::unexpected(action_id.error());
    }
    auto cache = build_detail::disk_cache{source};
    auto cache_opened = co_await cache.open();
    if (!cache_opened) {
        co_return std::unexpected(cache_opened.error());
    }
    auto candidate = cache.candidate_key(input, *sandbox_id);
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }
    auto cached = build_detail::cache_lookup{};
    if (lookup_cache) {
        auto lookup = co_await cache.lookup(input, *sandbox_id);
        if (!lookup) {
            co_return std::unexpected(lookup.error());
        }
        cached = std::move(*lookup);
    }
    co_return prepared_action{std::move(input),
                              *action_id,
                              *sandbox_id,
                              *candidate,
                              *source_time,
                              cached.hit,
                              cached.why,
                              cached.bytes_served};
}

runtime::task<result_id>
finish_action(repository& source, const prepared_action& prepared, action_result value) {
    auto stored = co_await build_detail::store_result_with_provenance(source, value);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    repository_detail::build_access::record_cache_lookup(
        source, stored->value, cache_status::miss, prepared.why, 0);
    if (repository_detail::build_access::cache_publish_enabled(source)) {
        auto cache = build_detail::disk_cache{source};
        auto cache_opened = co_await cache.open();
        if (!cache_opened) {
            co_return std::unexpected(cache_opened.error());
        }
        auto published = co_await cache.publish(prepared.candidate, *stored, value);
        if (!published) {
            co_return std::unexpected(published.error());
        }
    }
    co_return *stored;
}

namespace {

struct action_query_state {
    repository* source{};
    prepared_action* prepared{};
};

auto query_bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = static_cast<std::byte>(value[index]);
    }
    return output;
}

auto query_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

runtime::task<std::vector<std::byte>>
action_result_query(query_context& context, std::vector<std::byte>, action_query_state* state) {
    auto source_input = co_await context.read(
        graph_input_key{graph_input_kind::working_tree_blob, "action source tree"});
    if (!source_input) {
        co_return std::unexpected(source_input.error());
    }
    auto toolchain_input =
        co_await context.read(graph_input_key{graph_input_kind::toolchain, "action toolchain"});
    if (!toolchain_input) {
        co_return std::unexpected(toolchain_input.error());
    }
    auto cache = build_detail::disk_cache{*state->source};
    auto cache_opened = co_await cache.open();
    if (!cache_opened) {
        co_return std::unexpected(cache_opened.error());
    }
    auto cached = co_await cache.lookup(state->prepared->input, state->prepared->sandbox);
    if (!cached) {
        co_return std::unexpected(cached.error());
    }
    state->prepared->why = cached->why;
    state->prepared->bytes_served = cached->bytes_served;
    if (cached->hit) {
        repository_detail::build_access::record_cache_lookup(
            *state->source, cached->hit->value, cache_status::hit, {}, cached->bytes_served);
        co_return query_bytes(to_string(cached->hit->value));
    }
    auto value = co_await run_action(*state->source,
                                     state->prepared->input,
                                     state->prepared->action,
                                     state->prepared->sandbox,
                                     state->prepared->observed_at);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    value->schedule = execution_schedule{.decision = execution_decision::local,
                                         .reason = "no remote executor configured"};
    auto stored = co_await finish_action(*state->source, *state->prepared, std::move(*value));
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    co_return query_bytes(to_string(stored->value));
}

runtime::task<bool> query_result_present(repository& source, result_id id) {
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (cache.divergent(id.value)) {
        co_return false;
    }
    auto value = co_await result(source, id);
    if (!value) {
        co_return false;
    }
    auto references =
        std::vector<digest>{value->action, value->stdout_output, value->stderr_output};
    if (value->provenance) {
        references.push_back(*value->provenance);
    }
    references.insert(references.end(), value->sboms.begin(), value->sboms.end());
    for (const auto& output : value->outputs) {
        references.push_back(output.blob);
    }
    for (const auto& reference : references) {
        auto present = co_await repository_detail::build_access::contains(source, reference);
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (!*present) {
            co_return false;
        }
    }
    co_return true;
}

} // namespace

runtime::task<result_id> execute(repository& source, action_input input) {
#if defined(_WIN32)
    co_return std::unexpected(
        failure(errc::unsupported,
                "sandboxed actions are unavailable on Windows until Cloud Files and job objects",
                "liquid Phase 6"));
#else
    auto prepared = co_await prepare_action(source, std::move(input), false);
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    auto root = repository_detail::build_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto state = action_query_state{&source, &*prepared};
    auto registry = query_registry{};
    auto registered = registry.add(
        {.name = "action_result",
         .compute = [&state](query_context& context, std::vector<std::byte> arguments) {
             return action_result_query(context, std::move(arguments), &state);
         }});
    if (!registered) {
        co_return std::unexpected(registered.error());
    }
    auto queries =
        co_await graph::open(*root / ".liquid-build" / "query-graph", std::move(registry));
    if (!queries) {
        co_return std::unexpected(queries.error());
    }
    auto source_set =
        co_await queries->set_input({graph_input_kind::working_tree_blob, "action source tree"},
                                    query_bytes(to_string(prepared->input.inputs)),
                                    durability::low);
    auto toolchain_set =
        co_await queries->set_input({graph_input_kind::toolchain, "action toolchain"},
                                    query_bytes(to_string(prepared->input.toolchain)),
                                    durability::high);
    if (!source_set || !toolchain_set) {
        co_return std::unexpected(!source_set ? source_set.error() : toolchain_set.error());
    }
    const auto key = query_key{"action_result", query_bytes(to_string(prepared->candidate))};
    const auto executions = queries->statistics().executions;
    auto demanded = co_await queries->demand(key);
    if (!demanded) {
        co_return std::unexpected(demanded.error());
    }
    auto id = parse_digest(query_text(demanded->bytes));
    if (!id) {
        co_return std::unexpected(id.error());
    }
    if (queries->statistics().executions == executions) {
        auto present = co_await query_result_present(source, result_id{*id});
        if (!present) {
            co_return std::unexpected(present.error());
        }
        if (!*present) {
            auto invalidated = queries->mark_changed(key);
            if (!invalidated) {
                co_return std::unexpected(invalidated.error());
            }
            demanded = co_await queries->demand(key);
            if (!demanded) {
                co_return std::unexpected(demanded.error());
            }
            id = parse_digest(query_text(demanded->bytes));
            if (!id) {
                co_return std::unexpected(id.error());
            }
        } else {
            auto value = co_await result(source, result_id{*id});
            if (!value) {
                co_return std::unexpected(value.error());
            }
            auto served = co_await build_detail::output_bytes(source, *value);
            if (!served) {
                co_return std::unexpected(served.error());
            }
            repository_detail::build_access::record_cache_lookup(
                source, *id, cache_status::hit, {}, *served);
        }
    }
    co_return result_id{*id};
#endif
}

runtime::task<result_id>
record_remote_result(repository& source, action_input input, action_result value) {
    auto valid = validate(input, false);
    if (!valid) {
        co_return std::unexpected(valid.error());
    }
    for (const auto& output : value.outputs) {
        auto bytes = co_await repository_detail::build_access::get(source, output.blob);
        if (!bytes) {
            co_return std::unexpected(bytes.error());
        }
        auto record = objects::decode_blob(*bytes);
        if (!record || !record->remote) {
            co_return std::unexpected(failure(errc::invalid_argument,
                                              "remote result output has no remote location",
                                              output.path));
        }
    }
    auto sandbox = sandbox_detail::definition(input.platform);
    if (!sandbox) {
        co_return std::unexpected(sandbox.error());
    }
    auto sandbox_bytes = objects::encode(*sandbox);
    if (!sandbox_bytes) {
        co_return std::unexpected(sandbox_bytes.error());
    }
    auto sandbox_id =
        co_await repository_detail::build_access::put_record(source, std::move(*sandbox_bytes));
    if (!sandbox_id) {
        co_return std::unexpected(sandbox_id.error());
    }
    auto action_bytes = objects::encode(objects::action_record{input, *sandbox_id});
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action_id =
        co_await repository_detail::build_access::put_record(source, std::move(*action_bytes));
    if (!action_id) {
        co_return std::unexpected(action_id.error());
    }
    value.action = *action_id;
    if (value.worker.empty()) {
        value.worker = "remote:" + input.platform.os + "/" + input.platform.arch;
    }
    if (value.observed_at == std::chrono::sys_seconds{}) {
        value.observed_at =
            std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    }
    auto stored = co_await build_detail::store_result_with_provenance(source, value);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    if (repository_detail::build_access::cache_publish_enabled(source)) {
        auto cache = build_detail::disk_cache{source};
        auto opened = co_await cache.open();
        if (!opened) {
            co_return std::unexpected(opened.error());
        }
        auto candidate = cache.candidate_key(input, *sandbox_id);
        if (!candidate) {
            co_return std::unexpected(candidate.error());
        }
        auto published = co_await cache.publish(*candidate, *stored, value);
        if (!published) {
            co_return std::unexpected(published.error());
        }
    }
    co_return *stored;
}

runtime::task<action_result> result(repository& source, result_id id) {
    auto bytes = co_await repository_detail::build_access::get(source, id.value);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    auto decoded = objects::decode_result(*bytes);
    if (!decoded)
        co_return std::unexpected(decoded.error());
    repository_detail::build_access::apply_cache_lookup(source, id.value, *decoded);
    auto cache = build_detail::disk_cache{source};
    auto opened = co_await cache.open();
    if (!opened)
        co_return std::unexpected(opened.error());
    if (cache.divergent(id.value))
        decoded->cache = cache_status::divergent;
    co_return decoded;
}

} // namespace liquid::build
