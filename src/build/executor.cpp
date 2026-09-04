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
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
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

auto validate(const action_input& input) -> liquid::result<void> {
    if (input.command.empty() || input.command.front().empty())
        return std::unexpected(failure(errc::invalid_argument, "action command is empty"));
    const auto host = host_platform();
    if (input.platform.os != host.os || input.platform.arch != host.arch) {
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
    const auto sandbox_started = std::chrono::steady_clock::now();
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
                                                  .timeout = input.timeout};
    build_detail::testing::record_sandbox_spawn();
    const auto wall_started =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto launched = co_await runtime::offload(
        [launch_request] { return sandbox_detail::launch(launch_request); });
    const auto execution_finished = std::chrono::steady_clock::now();
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
    if (auto violation = denied_write(observed)) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(
            failure(errc::unauthorized, "action wrote outside declared outputs", *violation));
    }
    const auto upload_started = std::chrono::steady_clock::now();
    auto stdout_id = co_await repository_detail::build_access::put_file(source, paths.stdout_path);
    auto stderr_id = co_await repository_detail::build_access::put_file(source, paths.stderr_path);
    auto outputs = co_await collect_outputs(source, input, paths);
    auto read_manifest = manifest(observed);
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
    const auto upload_finished = std::chrono::steady_clock::now();
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
                       execution_finished - sandbox_started),
                   .exec = std::chrono::duration_cast<std::chrono::milliseconds>(
                       execution_finished - sandbox_started),
                   .upload = std::chrono::duration_cast<std::chrono::milliseconds>(upload_finished -
                                                                                   upload_started)},
        .worker = input.platform.os + "/" + input.platform.arch,
        .observed_at = observed_at};
    output.started_at = wall_started;
    output.finished_at = wall_finished;
    auto signed_result = co_await repository_detail::build_access::sign_result(source, output);
    if (!signed_result) {
        static_cast<void>(co_await runtime::offload([paths] { return remove_run(paths); }));
        co_return std::unexpected(signed_result.error());
    }
    auto removed = co_await runtime::offload([paths] { return remove_run(paths); });
    if (!removed) {
        co_return std::unexpected(removed.error());
    }
    if (!*removed) {
        co_return std::unexpected(removed->error());
    }
    co_return output;
}

runtime::task<result_id> execute(repository& source, action_input input) {
#if defined(_WIN32)
    co_return std::unexpected(
        failure(errc::unsupported,
                "sandboxed actions are unavailable on Windows until Cloud Files and job objects",
                "liquid Phase 6"));
#else
    auto valid = validate(input);
    if (!valid)
        co_return std::unexpected(valid.error());
    auto source_time = co_await repository_detail::build_access::change_time(source, input.inputs);
    if (!source_time)
        co_return std::unexpected(source_time.error());
    auto sandbox = sandbox_detail::definition(input.platform);
    if (!sandbox)
        co_return std::unexpected(sandbox.error());
    auto sandbox_bytes = objects::encode(*sandbox);
    if (!sandbox_bytes)
        co_return std::unexpected(sandbox_bytes.error());
    auto sandbox_id =
        co_await repository_detail::build_access::put_record(source, std::move(*sandbox_bytes));
    if (!sandbox_id)
        co_return std::unexpected(sandbox_id.error());
    auto action_bytes = objects::encode(objects::action_record{input, *sandbox_id});
    if (!action_bytes)
        co_return std::unexpected(action_bytes.error());
    auto action_id =
        co_await repository_detail::build_access::put_record(source, std::move(*action_bytes));
    if (!action_id)
        co_return std::unexpected(action_id.error());
    auto cache = build_detail::disk_cache{source};
    auto cache_opened = co_await cache.open();
    if (!cache_opened)
        co_return std::unexpected(cache_opened.error());
    auto candidate = cache.candidate_key(input, *sandbox_id);
    if (!candidate)
        co_return std::unexpected(candidate.error());
    auto cached = co_await cache.lookup(input, *sandbox_id);
    if (!cached)
        co_return std::unexpected(cached.error());
    if (cached->hit) {
        repository_detail::build_access::record_cache_lookup(
            source, cached->hit->value, cache_status::hit, {}, cached->bytes_served);
        co_return *cached->hit;
    }
    auto action_result_value =
        co_await run_action(source, input, *action_id, *sandbox_id, *source_time);
    if (!action_result_value)
        co_return std::unexpected(action_result_value.error());
    auto stored = co_await build_detail::store_result_with_provenance(source, *action_result_value);
    if (!stored)
        co_return std::unexpected(stored.error());
    repository_detail::build_access::record_cache_lookup(
        source, stored->value, cache_status::miss, cached->why, 0);
    if (repository_detail::build_access::cache_publish_enabled(source)) {
        auto published = co_await cache.publish(*candidate, *stored, *action_result_value);
        if (!published)
            co_return std::unexpected(published.error());
    }
    co_return *stored;
#endif
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
