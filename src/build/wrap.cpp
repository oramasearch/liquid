#include "liquid/build.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <mutex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <thread>

#if !defined(_WIN32)
#include <sys/resource.h>
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "build/budgets.hpp"
#include "build/cache.hpp"
#include "build/facts.hpp"
#include "build/materialise.hpp"
#include "build/policy.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "build/trace.hpp"
#include "build/verify.hpp"
#include "build/why.hpp"
#include "build/wrap.hpp"
#include "formats/json.hpp"
#include "liquid/graph.hpp"
#include "objects/records.hpp"
#include "sandbox/sandbox.hpp"
#include "vfs/backend.hpp"
#include "vfs/repository_access.hpp"

#ifndef LIQUID_WRAP_HOOK_FILENAME
#define LIQUID_WRAP_HOOK_FILENAME "libliquid-wrap-hook.so"
#endif

namespace liquid::build {
namespace {

constexpr auto virtual_root = std::string_view{"/liquid/work"};

static_assert(build_detail::restored_output_files_in_flight == 1);

auto peak_rss_bytes() -> std::uint64_t {
#if defined(_WIN32)
    return 0;
#else
    auto usage = rusage{};
    if (::getrusage(RUSAGE_SELF, &usage) != 0) {
        return 0;
    }
#if defined(__APPLE__)
    return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
    return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024;
#endif
#endif
}

struct observation_log {
    mutable std::mutex mutex;
    std::vector<vfs_detail::observed_request> requests;
};

struct traced_process {
    std::int64_t pid{};
    std::int64_t parent{};
    std::uint64_t key{};
    std::string executable;
    std::string cwd;
    std::vector<std::string> command;
    std::uint64_t started_ns{};
    std::uint64_t finished_ns{};
};

struct command_plan {
    std::vector<std::vector<std::string>> seeds;
    std::vector<std::string> driver;
};

struct launch_completion {
    liquid::result<sandbox_detail::launch_result> value;
};

struct saved_action {
    std::uint64_t key{};
    digest result{};
};

struct wrap_state {
    std::set<std::string> outputs;
    std::set<std::string> tools;
    std::vector<std::string> action_command;
    std::vector<saved_action> actions;
    std::optional<digest> outer_result;
};

struct cache_plan {
    struct condition {
        std::string path;
        std::uint64_t size{};
        std::uint64_t fingerprint{};
    };

    std::vector<saved_action> next_actions;
    std::set<std::uint64_t> missed_actions;
    std::set<std::uint64_t> reasoned_actions;
    std::map<std::uint64_t, int> processes;
    std::map<std::uint64_t, std::vector<condition>> conditions;
    std::vector<build_detail::lookup_record> lookups;
};

struct wrap_query_request {
    action_input input;
    digest sandbox;
    std::vector<observed_input> manifest;
};

struct wrap_query_state {
    repository* source{};
    build_detail::disk_cache* cache{};
    std::map<std::string, wrap_query_request> requests;
};

struct observed_run {
    std::filesystem::path directory;
    std::shared_ptr<observation_log> observations;
    std::vector<traced_process> processes;
    sandbox_detail::launch_result launch;
    std::chrono::sys_seconds started_at;
    std::chrono::sys_seconds finished_at;
    std::uint64_t trace_origin_ns{};
};

auto host_platform() -> platform;
runtime::task<action_result> load_result(repository& source, digest id);

auto wrap_error(errc code, std::string message, const std::filesystem::path& context = {})
    -> error {
    return {code, std::move(message), context.string()};
}

auto same_digest(const digest& left, const digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

auto hex(std::string_view input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto character : input) {
        const auto value = static_cast<unsigned char>(character);
        output.push_back(digits[value >> 4U]);
        output.push_back(digits[value & 0x0fU]);
    }
    return output;
}

auto file_condition(const std::filesystem::path& root, std::string path)
    -> liquid::result<cache_plan::condition> {
    auto input = std::ifstream{root / path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            wrap_error(errc::not_found, "conditional action input is missing", root / path));
    }
    auto fingerprint = std::uint64_t{1469598103934665603ULL};
    auto size = std::uint64_t{};
    auto buffer = std::array<char, 64 * 1024>{};
    while (input) {
        input.read(buffer.data(), static_cast<std::streamsize>(buffer.size()));
        const auto count = input.gcount();
        for (auto index = std::streamsize{}; index < count; ++index) {
            fingerprint ^= static_cast<unsigned char>(buffer[static_cast<std::size_t>(index)]);
            fingerprint *= 1099511628211ULL;
        }
        size += static_cast<std::uint64_t>(count);
    }
    if (!input.eof()) {
        return std::unexpected(
            wrap_error(errc::io, "conditional action input cannot be read", root / path));
    }
    return cache_plan::condition{std::move(path), size, fingerprint};
}

auto graph_bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        output[index] = static_cast<std::byte>(value[index]);
    }
    return output;
}

auto graph_text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

runtime::task<std::vector<std::byte>> wrap_action_result_query(query_context& context,
                                                               std::vector<std::byte> arguments,
                                                               wrap_query_state* state) {
    const auto candidate_text = graph_text(arguments);
    const auto request = state->requests.find(candidate_text);
    if (request == state->requests.end()) {
        co_return std::unexpected(wrap_error(errc::not_found, "wrap action query has no request"));
    }
    auto cache_entry = co_await context.read(
        graph_input_key{graph_input_kind::environment, "action-result/" + candidate_text});
    if (!cache_entry) {
        co_return std::unexpected(cache_entry.error());
    }
    for (const auto& input : request->second.manifest) {
        auto read =
            co_await context.read(graph_input_key{graph_input_kind::working_tree_blob, input.path});
        if (!read) {
            co_return std::unexpected(read.error());
        }
    }
    auto lookup = co_await state->cache->lookup(request->second.input, request->second.sandbox);
    if (!lookup) {
        co_return std::unexpected(lookup.error());
    }
    if (!lookup->hit) {
        co_return std::vector<std::byte>{};
    }
    co_return graph_bytes(to_string(lookup->hit->value));
}

runtime::task<build_detail::cache_lookup> demand_wrap_action(graph& queries,
                                                             wrap_query_state& state,
                                                             const action_input& input,
                                                             digest sandbox,
                                                             const action_result& previous) {
    auto candidate = state.cache->candidate_key(input, sandbox);
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }
    const auto candidate_text = to_string(*candidate);
    state.requests.insert_or_assign(candidate_text,
                                    wrap_query_request{input, sandbox, previous.manifest});
    auto cache_entry = co_await queries.set_input(
        {graph_input_kind::environment, "action-result/" + candidate_text},
        graph_bytes(to_string(previous.action)),
        durability::high);
    if (!cache_entry) {
        co_return std::unexpected(cache_entry.error());
    }
    auto demanded = co_await queries.demand({"action_result", graph_bytes(candidate_text)});
    if (!demanded) {
        co_return std::unexpected(demanded.error());
    }
    auto output = build_detail::cache_lookup{};
    if (demanded->bytes.empty()) {
        co_return output;
    }
    auto id = parse_digest(graph_text(demanded->bytes));
    if (!id) {
        co_return std::unexpected(id.error());
    }
    auto value = co_await load_result(*state.source, *id);
    if (!value) {
        auto changed =
            queries.mark_changed(query_key{"action_result", graph_bytes(candidate_text)});
        if (!changed) {
            co_return std::unexpected(changed.error());
        }
        auto refreshed = co_await queries.demand({"action_result", graph_bytes(candidate_text)});
        if (!refreshed) {
            co_return std::unexpected(refreshed.error());
        }
        if (refreshed->bytes.empty()) {
            co_return output;
        }
        id = parse_digest(graph_text(refreshed->bytes));
        if (!id) {
            co_return std::unexpected(id.error());
        }
        value = co_await load_result(*state.source, *id);
        if (!value) {
            co_return std::unexpected(value.error());
        }
    }
    auto avoided = co_await build_detail::output_bytes(*state.source, *value);
    if (!avoided) {
        co_return std::unexpected(avoided.error());
    }
    output.hit = result_id{*id};
    output.bytes_avoided = *avoided;
    output.bytes_served = *avoided;
    co_return output;
}

runtime::task<void> prime_wrap_actions(graph& queries,
                                       wrap_query_state& state,
                                       const wrap_state& saved,
                                       digest sandbox) {
    for (const auto& action : saved.actions) {
        auto previous = co_await load_result(*state.source, action.result);
        if (!previous) {
            co_return std::unexpected(previous.error());
        }
        auto action_bytes =
            co_await repository_detail::build_access::get(*state.source, previous->action);
        if (!action_bytes) {
            co_return std::unexpected(action_bytes.error());
        }
        auto decoded = objects::decode_action(*action_bytes);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        auto demanded =
            co_await demand_wrap_action(queries, state, decoded->input, sandbox, *previous);
        if (!demanded) {
            co_return std::unexpected(demanded.error());
        }
    }
    if (saved.outer_result) {
        auto previous = co_await load_result(*state.source, *saved.outer_result);
        if (!previous) {
            co_return std::unexpected(previous.error());
        }
        auto action_bytes =
            co_await repository_detail::build_access::get(*state.source, previous->action);
        if (!action_bytes) {
            co_return std::unexpected(action_bytes.error());
        }
        auto decoded = objects::decode_action(*action_bytes);
        if (!decoded) {
            co_return std::unexpected(decoded.error());
        }
        auto demanded =
            co_await demand_wrap_action(queries, state, decoded->input, sandbox, *previous);
        if (!demanded) {
            co_return std::unexpected(demanded.error());
        }
    }
    co_return liquid::result<void>{};
}

auto unhex(std::string_view input) -> liquid::result<std::string> {
    if (input.size() % 2 != 0)
        return std::unexpected(wrap_error(errc::corrupt, "invalid wrap state encoding"));
    const auto digit = [](char value) -> int {
        if (value >= '0' && value <= '9')
            return value - '0';
        if (value >= 'a' && value <= 'f')
            return value - 'a' + 10;
        if (value >= 'A' && value <= 'F')
            return value - 'A' + 10;
        return -1;
    };
    auto output = std::string{};
    output.reserve(input.size() / 2);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 2) {
        const auto high = digit(input[offset]);
        const auto low = digit(input[offset + 1]);
        if (high < 0 || low < 0)
            return std::unexpected(wrap_error(errc::corrupt, "invalid wrap state encoding"));
        output.push_back(static_cast<char>((high << 4U) | low));
    }
    return output;
}

auto split_tabs(std::string_view line) -> std::vector<std::string_view> {
    auto output = std::vector<std::string_view>{};
    auto offset = std::size_t{};
    for (;;) {
        const auto separator = line.find('\t', offset);
        output.push_back(line.substr(offset, separator - offset));
        if (separator == std::string_view::npos)
            break;
        offset = separator + 1;
    }
    return output;
}

auto read_state(const std::filesystem::path& path) -> liquid::result<wrap_state> {
    auto output = wrap_state{};
    if (!std::filesystem::exists(path))
        return output;
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        return std::unexpected(wrap_error(errc::io, "cannot read wrap state", path));
    auto line = std::string{};
    while (std::getline(input, line)) {
        const auto fields = split_tabs(line);
        if (fields.size() < 2)
            return std::unexpected(wrap_error(errc::corrupt, "invalid wrap state", path));
        if (fields[0] == "A" && fields.size() == 3) {
            auto key = std::uint64_t{};
            auto key_stream = std::istringstream{std::string{fields[1]}};
            key_stream >> std::hex >> key;
            auto id = parse_digest(fields[2]);
            if (!key_stream || !id)
                return std::unexpected(wrap_error(errc::corrupt, "invalid saved action", path));
            output.actions.push_back({key, *id});
            continue;
        }
        if (fields[0] == "R" && fields.size() == 2) {
            auto id = parse_digest(fields[1]);
            if (!id)
                return std::unexpected(wrap_error(errc::corrupt, "invalid outer action", path));
            output.outer_result = *id;
            continue;
        }
        if (fields[0] == "S") {
            continue;
        }
        auto value = unhex(fields[1]);
        if (!value)
            return std::unexpected(value.error());
        if (fields[0] == "O")
            output.outputs.insert(std::move(*value));
        else if (fields[0] == "T")
            output.tools.insert(std::move(*value));
        else if (fields[0] == "C")
            output.action_command.push_back(std::move(*value));
        else
            return std::unexpected(wrap_error(errc::corrupt, "unknown wrap state entry", path));
    }
    return output;
}

auto write_state(const std::filesystem::path& path, const wrap_state& state)
    -> liquid::result<void> {
    auto temporary = path;
    temporary += ".tmp";
    auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
    if (!output)
        return std::unexpected(wrap_error(errc::io, "cannot write wrap state", path));
    for (const auto& item : state.outputs)
        output << "O\t" << hex(item) << '\n';
    for (const auto& tool : state.tools)
        output << "T\t" << hex(tool) << '\n';
    for (const auto& argument : state.action_command)
        output << "C\t" << hex(argument) << '\n';
    for (const auto& action : state.actions)
        output << "A\t" << std::hex << action.key << '\t' << to_string(action.result) << '\n';
    if (state.outer_result)
        output << "R\t" << to_string(*state.outer_result) << '\n';
    output.close();
    if (!output)
        return std::unexpected(wrap_error(errc::io, "cannot write wrap state", path));
    auto code = std::error_code{};
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code)
        return std::unexpected(wrap_error(errc::io, code.message(), path));
    return {};
}

auto default_cache_directory(const std::filesystem::path& root)
    -> liquid::result<std::filesystem::path> {
    return build_detail::wrap_cache_directory(root);
}

auto wrap_hook_path() -> liquid::result<std::filesystem::path> {
    auto candidates = std::vector<std::filesystem::path>{};
    if (const auto* configured = std::getenv("LIQUID_WRAP_HOOK"))
        candidates.emplace_back(configured);
#if defined(__linux__)
    auto code = std::error_code{};
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", code);
    if (!code) {
        candidates.push_back(executable.parent_path() / LIQUID_WRAP_HOOK_FILENAME);
        candidates.push_back(executable.parent_path() / ".." / LIQUID_WRAP_HOOK_FILENAME);
        candidates.push_back(executable.parent_path() / "../libexec/liquid" /
                             LIQUID_WRAP_HOOK_FILENAME);
    }
#elif defined(__APPLE__)
    auto size = std::uint32_t{};
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    auto executable = std::string(size, '\0');
    if (_NSGetExecutablePath(executable.data(), &size) == 0) {
        executable.resize(std::strlen(executable.c_str()));
        const auto executable_directory = std::filesystem::path{executable}.parent_path();
        candidates.push_back(executable_directory / LIQUID_WRAP_HOOK_FILENAME);
        candidates.push_back(executable_directory / ".." / LIQUID_WRAP_HOOK_FILENAME);
        candidates.push_back(executable_directory / "../libexec/liquid" /
                             LIQUID_WRAP_HOOK_FILENAME);
    }
#endif
    for (const auto& candidate : candidates) {
        if (!candidate.empty() && std::filesystem::is_regular_file(candidate))
            return std::filesystem::canonical(candidate);
    }
    return std::unexpected(wrap_error(errc::not_found,
                                      "cannot locate the liquid wrap exec-time hook",
                                      LIQUID_WRAP_HOOK_FILENAME));
}

runtime::task<void> sync_sources(repository& source,
                                 const std::filesystem::path& root,
                                 const std::filesystem::path& cache,
                                 wrap_state& state,
                                 const std::vector<std::string>& command) {
    auto options = import_options{};
    options.message = "synchronise wrap sources";
    options.ignore_patterns = {
        ".git/**", ".liquid-store/**", "LIQUID_REPO", "LIQUID_JOURNAL", ".liquid-build/**"};
    for (const auto& output : state.outputs) {
        options.ignore_patterns.push_back(output);
        options.ignore_patterns.push_back(output + "/**");
    }
    const auto relative_cache = cache.lexically_relative(root).generic_string();
    if (!relative_cache.empty() && !relative_cache.starts_with("../")) {
        options.ignore_patterns.push_back(relative_cache);
        options.ignore_patterns.push_back(relative_cache + "/**");
    }
    for (auto index = std::size_t{}; index != command.size(); ++index) {
        auto build_directory = std::string_view{};
        if (command[index] == "--build" && index + 1 != command.size()) {
            build_directory = command[index + 1];
        } else if (command[index].starts_with("--build=")) {
            build_directory = std::string_view{command[index]}.substr(8);
        }
        if (build_directory.empty()) {
            continue;
        }
        const auto relative =
            std::filesystem::path{build_directory}.lexically_normal().generic_string();
        if (relative.empty() || relative.starts_with("../") || relative.starts_with('/')) {
            continue;
        }
        options.ignore_patterns.push_back("!" + relative);
        options.ignore_patterns.push_back("!" + relative + "/**");
    }
    auto imported = co_await source.import_tree(root, std::move(options));
    if (!imported) {
        co_return std::unexpected(imported.error());
    }
    co_return liquid::result<void>{};
}

runtime::task<std::vector<std::byte>> blob_contents(repository& source, digest id) {
    auto raw = co_await repository_detail::build_access::get(source, id);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto blob = objects::decode_blob(*raw);
    if (!blob)
        co_return std::unexpected(blob.error());
    if (blob->content.alg == algorithm::blake3 || blob->content.alg == algorithm::sha256 ||
        blob->content.alg == algorithm::blake3_keyed) {
        auto contents = co_await repository_detail::build_access::get(source, blob->content);
        if (!contents) {
            co_return std::unexpected(contents.error());
        }
        if (contents->size() != blob->size) {
            co_return std::unexpected(wrap_error(errc::corrupt, "cached output size is invalid"));
        }
        co_return contents;
    }
    auto output = std::vector<std::byte>{};
    output.reserve(static_cast<std::size_t>(blob->size));
    for (const auto& chunk : blob->chunks) {
        auto bytes = co_await repository_detail::build_access::get(source, chunk);
        if (!bytes)
            co_return std::unexpected(bytes.error());
        output.insert(output.end(), bytes->begin(), bytes->end());
    }
    if (output.size() != blob->size)
        co_return std::unexpected(wrap_error(errc::corrupt, "cached output size is invalid"));
    co_return output;
}

runtime::task<void> stream_store_object(repository& source,
                                        digest id,
                                        std::uint64_t size,
                                        const std::shared_ptr<std::ofstream>& output,
                                        const std::filesystem::path& target) {
    auto offset = std::uint64_t{};
    while (offset != size) {
        const auto remaining = size - offset;
        const auto count = static_cast<std::size_t>(
            std::min<std::uint64_t>(remaining, build_detail::restored_output_buffer_bytes));
        auto bytes = std::vector<std::byte>(count);
        auto read = co_await repository_detail::build_access::read(source, id, offset, bytes);
        if (!read) {
            co_return std::unexpected(read.error());
        }
        if (*read == 0 || *read > bytes.size()) {
            co_return std::unexpected(
                wrap_error(errc::corrupt, "cached output is truncated", target));
        }
        bytes.resize(static_cast<std::size_t>(*read));
        auto written = co_await runtime::offload([output, bytes = std::move(bytes), target] {
            output->write(reinterpret_cast<const char*>(bytes.data()),
                          static_cast<std::streamsize>(bytes.size()));
            if (!*output) {
                return liquid::result<void>{
                    std::unexpected(wrap_error(errc::io, "cannot materialise output", target))};
            }
            return liquid::result<void>{};
        });
        if (!written) {
            co_return std::unexpected(written.error());
        }
        if (!*written) {
            co_return std::unexpected(written->error());
        }
        offset += *read;
    }
    co_return liquid::result<void>{};
}

runtime::task<void> stream_blob_to_file(repository& source,
                                        digest id,
                                        const std::filesystem::path& target,
                                        std::uint32_t mode) {
    auto raw = co_await repository_detail::build_access::get(source, id);
    if (!raw) {
        co_return std::unexpected(raw.error());
    }
    auto blob = objects::decode_blob(*raw);
    if (!blob) {
        co_return std::unexpected(blob.error());
    }
    auto created =
        co_await runtime::offload([target] -> liquid::result<std::shared_ptr<std::ofstream>> {
            auto code = std::error_code{};
            std::filesystem::create_directories(target.parent_path(), code);
            if (code) {
                return std::unexpected(wrap_error(errc::io, code.message(), target));
            }
            auto output =
                std::make_shared<std::ofstream>(target, std::ios::binary | std::ios::trunc);
            if (!*output) {
                return std::unexpected(wrap_error(errc::io, "cannot materialise output", target));
            }
            return output;
        });
    if (!created) {
        co_return std::unexpected(created.error());
    }
    if (!*created) {
        co_return std::unexpected(created->error());
    }
    const auto& output = **created;
    if (blob->content.alg == algorithm::blake3 || blob->content.alg == algorithm::sha256 ||
        blob->content.alg == algorithm::blake3_keyed) {
        auto copied =
            co_await stream_store_object(source, blob->content, blob->size, output, target);
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
    } else {
        for (auto index = std::size_t{}; index != blob->chunks.size(); ++index) {
            auto copied = co_await stream_store_object(
                source, blob->chunks[index], blob->chunk_sizes[index], output, target);
            if (!copied) {
                co_return std::unexpected(copied.error());
            }
        }
    }
    auto closed = co_await runtime::offload([output, target, mode] -> liquid::result<void> {
        output->close();
        if (!*output) {
            return std::unexpected(wrap_error(errc::io, "cannot materialise output", target));
        }
        auto code = std::error_code{};
        std::filesystem::permissions(target,
                                     static_cast<std::filesystem::perms>(mode & 0777U),
                                     std::filesystem::perm_options::replace,
                                     code);
        if (code) {
            return std::unexpected(wrap_error(errc::io, code.message(), target));
        }
        return {};
    });
    if (!closed) {
        co_return std::unexpected(closed.error());
    }
    if (!*closed) {
        co_return std::unexpected(closed->error());
    }
    co_return liquid::result<void>{};
}

runtime::task<void> materialise_result(repository& source, const action_result& value) {
    for (const auto& output : value.outputs) {
        auto root = repository_detail::build_access::root_path(source);
        if (!root) {
            co_return std::unexpected(root.error());
        }
        static auto restored_index = std::atomic_uint64_t{};
        const auto temporary =
            *root / ("restored-output-" + std::to_string(restored_index.fetch_add(1)));
        auto copied = co_await stream_blob_to_file(source, output.blob, temporary, output.mode);
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
        auto written = co_await source.write_file(output.path, temporary);
        auto code = std::error_code{};
        std::filesystem::remove(temporary, code);
        if (!written)
            co_return std::unexpected(written.error());
        auto mode = co_await repository_detail::vfs_access::set_mode(
            source, output.path, static_cast<std::uint32_t>(S_IFREG) | output.mode);
        if (!mode)
            co_return std::unexpected(mode.error());
    }
    co_return liquid::result<void>{};
}

runtime::task<void> materialise_result(repository& source,
                                       const action_result& value,
                                       const std::filesystem::path& working_tree) {
    for (const auto& output : value.outputs) {
        const auto target = working_tree / output.path;
        auto copied = co_await stream_blob_to_file(source, output.blob, target, output.mode);
        if (!copied)
            co_return std::unexpected(copied.error());
    }
    co_return liquid::result<void>{};
}

runtime::task<void> copy_outputs(repository& source,
                                 const std::filesystem::path& root,
                                 const std::set<std::string>& outputs) {
    for (const auto& path : outputs) {
        auto entry = co_await repository_detail::vfs_access::entry(source, path);
        if (!entry) {
            if (entry.error().code == errc::not_found)
                continue;
            co_return std::unexpected(entry.error());
        }
        auto target = root / path;
        auto copied = co_await stream_blob_to_file(source, entry->object, target, entry->mode);
        if (!copied)
            co_return std::unexpected(copied.error());
    }
    co_return liquid::result<void>{};
}

auto parse_trace(const std::filesystem::path& path) -> liquid::result<std::vector<traced_process>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input)
        return std::unexpected(wrap_error(errc::io, "cannot read wrap process trace", path));
    auto by_pid = std::map<std::int64_t, traced_process>{};
    auto order = std::vector<std::int64_t>{};
    auto line = std::string{};
    while (std::getline(input, line)) {
        const auto fields = split_tabs(line);
        if (fields.size() == 3 && fields[0] == "X") {
            auto pid = std::int64_t{};
            auto finished = std::uint64_t{};
            auto numbers =
                std::istringstream{std::string{fields[1]} + " " + std::string{fields[2]}};
            numbers >> pid >> finished;
            if (numbers && by_pid.contains(pid)) {
                by_pid.at(pid).finished_ns = finished;
            }
            continue;
        }
        if (fields.size() != 6 && fields.size() != 7)
            continue;
        auto process = traced_process{};
        auto numbers = std::istringstream{std::string{fields[0]} + " " + std::string{fields[1]}};
        numbers >> process.pid >> process.parent;
        auto key = std::istringstream{std::string{fields[2]}};
        key >> std::hex >> process.key;
        auto executable = unhex(fields[3]);
        auto cwd = unhex(fields[4]);
        auto arguments = unhex(fields[5]);
        if (!numbers || !key || !executable || !cwd || !arguments)
            continue;
        if (fields.size() == 7) {
            auto started = std::istringstream{std::string{fields[6]}};
            started >> process.started_ns;
            if (!started) {
                continue;
            }
        }
        process.executable = std::move(*executable);
        process.cwd = std::move(*cwd);
        auto offset = std::size_t{};
        while (offset < arguments->size()) {
            const auto end = arguments->find('\0', offset);
            process.command.push_back(arguments->substr(offset, end - offset));
            if (end == std::string::npos)
                break;
            offset = end + 1;
        }
        if (!by_pid.contains(process.pid))
            order.push_back(process.pid);
        by_pid.insert_or_assign(process.pid, std::move(process));
    }
    auto output = std::vector<traced_process>{};
    for (const auto pid : order)
        output.push_back(std::move(by_pid.at(pid)));
    return output;
}

auto relative_cwd(std::string_view cwd) -> std::string {
    if (cwd == virtual_root)
        return {};
    const auto prefix = std::string{virtual_root} + "/";
    return cwd.starts_with(prefix) ? std::string{cwd.substr(prefix.size())} : std::string{cwd};
}

auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'')
            output += "'\\''";
        else
            output += character;
    }
    output += '\'';
    return output;
}

auto json_field(const formats::json::value::object& object, std::string_view name)
    -> const formats::json::value* {
    const auto found = std::ranges::find(
        object, name, [](const auto& item) { return std::string_view{item.first}; });
    return found == object.end() ? nullptr : &found->second;
}

auto planned_commands(const std::filesystem::path& working_tree,
                      const std::vector<std::string>& driver) -> liquid::result<command_plan> {
    const auto database = working_tree / "compile_commands.json";
    if (!std::filesystem::is_regular_file(database))
        return command_plan{{}, driver};
    auto input = std::ifstream{database, std::ios::binary};
    auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    if (!input && !input.eof())
        return std::unexpected(wrap_error(errc::io, "cannot read compile database", database));
    auto parsed = formats::json::parse(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    const auto* entries = std::get_if<formats::json::value::array>(&parsed->data);
    if (!entries || entries->empty())
        return command_plan{{}, driver};
    auto output = command_plan{};
    for (const auto& entry : *entries) {
        const auto* object = std::get_if<formats::json::value::object>(&entry.data);
        if (!object)
            return std::unexpected(wrap_error(
                errc::invalid_argument, "compile database entry is not an object", database));
        auto directory = std::string{virtual_root};
        if (const auto* field = json_field(*object, "directory")) {
            const auto* value = std::get_if<std::string>(&field->data);
            if (!value)
                return std::unexpected(wrap_error(
                    errc::invalid_argument, "compile database directory is not text", database));
            auto path = std::filesystem::path{*value};
            if (path.is_relative())
                path = database.parent_path() / path;
            const auto relative = path.lexically_normal().lexically_relative(working_tree);
            directory = relative.empty() || relative == "."
                            ? std::string{virtual_root}
                            : std::string{virtual_root} + "/" + relative.generic_string();
        }
        auto command = std::string{};
        auto output_directory = std::string{};
        if (const auto* field = json_field(*object, "output")) {
            const auto* value = std::get_if<std::string>(&field->data);
            if (!value)
                return std::unexpected(wrap_error(
                    errc::invalid_argument, "compile database output is not text", database));
            output_directory = std::filesystem::path{*value}.parent_path().generic_string();
        }
        if (const auto* field = json_field(*object, "arguments")) {
            const auto* values = std::get_if<formats::json::value::array>(&field->data);
            if (!values)
                return std::unexpected(wrap_error(errc::invalid_argument,
                                                  "compile database arguments is not an array",
                                                  database));
            for (const auto& item : *values) {
                const auto* value = std::get_if<std::string>(&item.data);
                if (!value)
                    return std::unexpected(wrap_error(
                        errc::invalid_argument, "compile database argument is not text", database));
                if (!command.empty())
                    command += ' ';
                command += shell_quote(*value);
            }
        } else if (const auto* field = json_field(*object, "command")) {
            const auto* value = std::get_if<std::string>(&field->data);
            if (!value)
                return std::unexpected(wrap_error(
                    errc::invalid_argument, "compile database command is not text", database));
            command = *value;
        }
        if (!command.empty()) {
            auto body = std::string{};
            if (!output_directory.empty())
                body = "mkdir -p " + shell_quote(output_directory) + " && ";
            output.seeds.push_back(
                {"/bin/sh", "-c", "cd " + shell_quote(directory) + " && " + body + command});
        }
    }
    if (output.seeds.empty())
        return command_plan{{}, driver};
    const auto database_argument = database.lexically_relative(working_tree).generic_string();
    const auto database_only =
        driver.size() == 1 && std::filesystem::path{driver.front()} == database_argument;
    if (!database_only)
        output.driver = driver;
    return output;
}

auto manifest_for(const observation_log& observed,
                  std::int64_t pid,
                  const std::set<std::string>& written)
    -> liquid::result<std::vector<observed_input>> {
    auto entries = std::map<std::string, digest>{};
    auto lock = std::scoped_lock{observed.mutex};
    for (const auto& request : observed.requests) {
        if ((pid >= 0 && request.pid != pid) || request.denied || !request.content ||
            request.path.empty() || written.contains(request.path) ||
            (request.operation != vfs_detail::observed_operation::open &&
             request.operation != vfs_detail::observed_operation::read)) {
            continue;
        }
        const auto found = entries.find(request.path);
        if (found != entries.end() && !same_digest(found->second, *request.content))
            return std::unexpected(wrap_error(
                errc::internal, "wrapped input changed while it was read", request.path));
        entries.insert_or_assign(request.path, *request.content);
    }
    auto output = std::vector<observed_input>{};
    for (const auto& [path, content] : entries)
        output.push_back({path, content});
    return output;
}

auto writes_by_process(const observation_log& observed)
    -> std::map<std::int64_t, std::set<std::string>> {
    auto output = std::map<std::int64_t, std::set<std::string>>{};
    auto lock = std::scoped_lock{observed.mutex};
    for (const auto& request : observed.requests) {
        if (request.operation == vfs_detail::observed_operation::write && !request.denied &&
            !request.path.empty()) {
            output[request.pid].insert(request.path);
        }
    }
    return output;
}

auto has_writing_descendant(const traced_process& process,
                            const std::vector<traced_process>& processes,
                            const std::map<std::int64_t, std::set<std::string>>& writes) -> bool {
    auto parents = std::map<std::int64_t, std::int64_t>{};
    for (const auto& item : processes)
        parents.insert_or_assign(item.pid, item.parent);
    for (const auto& [pid, paths] : writes) {
        if (paths.empty() || pid == process.pid)
            continue;
        auto current = pid;
        auto visited = std::set<std::int64_t>{};
        while (parents.contains(current) && visited.insert(current).second) {
            current = parents.at(current);
            if (current == process.pid)
                return true;
        }
    }
    return false;
}

runtime::task<std::vector<action_output>>
collect_action_outputs(repository& source, const std::set<std::string>& paths) {
    auto output = std::vector<action_output>{};
    for (const auto& path : paths) {
        auto entry = co_await repository_detail::vfs_access::entry(source, path);
        if (!entry) {
            if (entry.error().code == errc::not_found)
                continue;
            co_return std::unexpected(entry.error());
        }
        if (entry->kind == node_kind::blob)
            output.push_back({path, entry->mode & 0777U, entry->object});
    }
    co_return output;
}

runtime::task<saved_action> publish_action(repository& source,
                                           build_detail::disk_cache& cache,
                                           const traced_process& process,
                                           const std::map<std::string, std::string>& environment,
                                           digest input_tree,
                                           digest toolchain,
                                           digest sandbox,
                                           const observation_log& observed,
                                           const std::set<std::string>& writes,
                                           digest empty_blob,
                                           int exit,
                                           std::chrono::sys_seconds started_at,
                                           std::chrono::sys_seconds finished_at,
                                           bool publish) {
    const auto hash_started = std::chrono::steady_clock::now();
    auto output_paths = co_await collect_action_outputs(source, writes);
    if (!output_paths)
        co_return std::unexpected(output_paths.error());
    auto manifest = manifest_for(observed, process.pid, writes);
    if (!manifest)
        co_return std::unexpected(manifest.error());
    auto input = action_input{.command = process.command,
                              .cwd = relative_cwd(process.cwd),
                              .env = environment,
                              .outputs = {writes.begin(), writes.end()},
                              .inputs = input_tree,
                              .toolchain = toolchain,
                              .platform = host_platform(),
                              .timeout = {}};
    auto action_bytes = objects::encode(objects::action_record{input, sandbox});
    if (!action_bytes)
        co_return std::unexpected(action_bytes.error());
    const auto hash_finished = std::chrono::steady_clock::now();
    auto action =
        co_await repository_detail::build_access::put_record(source, std::move(*action_bytes));
    if (!action)
        co_return std::unexpected(action.error());
    const auto store_finished = std::chrono::steady_clock::now();
    const auto execution = process.finished_ns >= process.started_ns
                               ? std::chrono::nanoseconds{process.finished_ns - process.started_ns}
                               : std::chrono::nanoseconds{};
    auto value = action_result{
        .action = *action,
        .exit = exit,
        .outputs = std::move(*output_paths),
        .stdout_output = empty_blob,
        .stderr_output = empty_blob,
        .manifest = std::move(*manifest),
        .timing = {.exec = std::chrono::duration_cast<std::chrono::milliseconds>(execution),
                   .hash = std::chrono::duration_cast<std::chrono::milliseconds>(hash_finished -
                                                                                 hash_started),
                   .store = std::chrono::duration_cast<std::chrono::milliseconds>(store_finished -
                                                                                  hash_finished)},
        .worker = input.platform.os + "/" + input.platform.arch,
        .observed_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
    value.schedule = execution_schedule{.decision = execution_decision::local,
                                        .reason = "wrap has no remote executor configured"};
    value.started_at = started_at;
    value.finished_at = finished_at;
    auto stored_result = co_await build_detail::store_result_with_provenance(source, value);
    if (!stored_result)
        co_return std::unexpected(stored_result.error());
    if (publish) {
        auto candidate = cache.candidate_key(input, sandbox);
        if (!candidate)
            co_return std::unexpected(candidate.error());
        auto published = co_await cache.publish(*candidate, *stored_result, value);
        if (!published)
            co_return std::unexpected(published.error());
    }
    co_return saved_action{process.key, stored_result->value};
}

auto host_platform() -> platform {
#if defined(__linux__)
    constexpr auto os = "linux";
#elif defined(__APPLE__)
    constexpr auto os = "macos";
#else
    constexpr auto os = "windows";
#endif
#if defined(__x86_64__) || defined(_M_X64)
    constexpr auto arch = "x86_64";
#else
    constexpr auto arch = "arm64";
#endif
    return {os, arch};
}

auto fixed_environment(const wrap_options& options) -> std::map<std::string, std::string> {
    auto environment = std::map<std::string, std::string>{{"PATH", "/usr/local/bin:/usr/bin:/bin"},
                                                          {"HOME", "/tmp/home"},
                                                          {"TMPDIR", "/tmp"},
                                                          {"LANG", "C.UTF-8"},
                                                          {"TZ", "UTC"},
                                                          {"SOURCE_DATE_EPOCH", "0"}};
    if (const auto* compiler = std::getenv("CC"))
        environment["CC"] = compiler;
    if (const auto* compiler = std::getenv("CXX"))
        environment["CXX"] = compiler;
    if (options.jobs != 0) {
        environment["CMAKE_BUILD_PARALLEL_LEVEL"] = std::to_string(options.jobs);
        environment["MAKEFLAGS"] = "-j" + std::to_string(options.jobs);
    }
    return environment;
}

auto apply_cached_variables(std::vector<std::string>& command,
                            std::map<std::string, std::string>& environment,
                            const std::map<std::string, std::string>& variables) -> void {
    auto cmake_configure = !command.empty() &&
                           std::filesystem::path{command.front()}.filename() == "cmake" &&
                           std::ranges::find(command, "--build") == command.end();
    for (const auto& [name, answer] : variables) {
        if (name.starts_with("ac_cv_")) {
            environment.insert_or_assign(name, answer);
        } else if (cmake_configure) {
            command.push_back("-D" + name + "=" + answer);
        }
    }
}

auto probe_program_digest(const observation_log& observed, std::int64_t pid)
    -> std::optional<digest> {
    auto lock = std::scoped_lock{observed.mutex};
    for (const auto& request : observed.requests) {
        if (request.pid != pid || !request.content) {
            continue;
        }
        const auto path = std::filesystem::path{request.path};
        const auto extension = path.extension().string();
        if (path.filename().string().starts_with("conftest.") ||
            (request.path.contains("CMakeScratch/TryCompile-") &&
             (extension == ".c" || extension == ".cc" || extension == ".cpp" ||
              extension == ".cxx"))) {
            return request.content;
        }
    }
    return std::nullopt;
}

runtime::task<std::string>
probe_program_text(repository& source, const observation_log& observed, std::int64_t pid) {
    const auto program = probe_program_digest(observed, pid);
    if (!program) {
        co_return std::string{};
    }
    auto contents = co_await blob_contents(source, *program);
    if (!contents) {
        co_return std::unexpected(contents.error());
    }
    co_return std::string{reinterpret_cast<const char*>(contents->data()), contents->size()};
}

auto shareable_probe_program(std::string_view program) -> bool {
    return program.contains("#include <") ||
           program.contains("Override any GCC internal prototype") ||
           program.contains("CMAKE_HAVE_") || program.contains("HAVE_");
}

runtime::task<digest> store_toolchain(repository& source,
                                      const std::set<std::string>& executables) {
    auto value = tree{};
    auto index = std::uint64_t{};
    for (const auto& executable : executables) {
        if (!std::filesystem::is_regular_file(executable))
            continue;
        auto blob = co_await repository_detail::build_access::put_file(source, executable);
        if (!blob)
            co_return std::unexpected(blob.error());
        auto status = std::filesystem::status(executable);
        value.entries.push_back({.name = std::to_string(index++) + "-" + hex(executable),
                                 .mode = static_cast<std::uint32_t>(status.permissions()) & 0777U,
                                 .kind = node_kind::blob,
                                 .id = *blob,
                                 .ref = *blob});
    }
    auto encoded = objects::encode(value);
    if (!encoded)
        co_return std::unexpected(encoded.error());
    co_return co_await repository_detail::build_access::put_record(source, std::move(*encoded));
}

auto read_toolchain(const std::filesystem::path& path) -> liquid::result<std::optional<digest>> {
    if (!std::filesystem::exists(path))
        return std::nullopt;
    auto input = std::ifstream{path};
    auto text = std::string{};
    std::getline(input, text);
    if (!input)
        return std::unexpected(wrap_error(errc::io, "cannot read wrap toolchain pin", path));
    auto parsed = parse_digest(text);
    if (!parsed)
        return std::unexpected(parsed.error());
    return *parsed;
}

auto write_toolchain(const std::filesystem::path& path, digest value) -> liquid::result<void> {
    auto output = std::ofstream{path, std::ios::trunc};
    if (!output)
        return std::unexpected(wrap_error(errc::io, "cannot write wrap toolchain pin", path));
    output << to_string(value) << '\n';
    if (!output)
        return std::unexpected(wrap_error(errc::io, "cannot write wrap toolchain pin", path));
    return {};
}

runtime::task<digest> store_sandbox(repository& source) {
    auto encoded = objects::encode(wrap_sandbox_definition());
    if (!encoded)
        co_return std::unexpected(encoded.error());
    co_return co_await repository_detail::build_access::put_record(source, std::move(*encoded));
}

runtime::task<action_result> load_result(repository& source, digest id) {
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    co_return objects::decode_result(*bytes);
}

runtime::task<objects::action_record> load_action(repository& source, digest id) {
    auto bytes = co_await repository_detail::build_access::get(source, id);
    if (!bytes)
        co_return std::unexpected(bytes.error());
    co_return objects::decode_action(*bytes);
}

auto remove_run(const std::filesystem::path& path) -> liquid::result<void> {
    const auto parent = path.parent_path().filename();
    if (path.filename().empty() || (parent != "runs" && parent != "verify"))
        return std::unexpected(wrap_error(errc::internal, "invalid wrap run path", path));
    auto code = std::error_code{};
    std::filesystem::remove_all(path, code);
    if (code)
        return std::unexpected(wrap_error(errc::io, code.message(), path));
    return {};
}

runtime::task<sandbox_detail::launch_result>
launch_wrapped_process(sandbox_detail::wrap_launch_request request) {
    auto launched = co_await runtime::offload(
        [request = std::move(request)] { return sandbox_detail::launch_wrap(request); });
    if (!launched)
        co_return std::unexpected(launched.error());
    if (!*launched)
        co_return std::unexpected(launched->error());
    co_return std::move(**launched);
}

boost::capy::io_task<launch_completion>
launch_wrapped_io(sandbox_detail::wrap_launch_request request) {
    auto launched = co_await launch_wrapped_process(std::move(request));
    co_return boost::capy::io_result<launch_completion>{{}, launch_completion{std::move(launched)}};
}

runtime::task<std::vector<sandbox_detail::launch_result>>
launch_seed_processes(const sandbox_detail::wrap_launch_request& base,
                      const std::vector<std::vector<std::string>>& commands,
                      std::uint32_t requested_jobs) {
    auto output = std::vector<sandbox_detail::launch_result>{};
    const auto jobs =
        requested_jobs == 0 ? std::max(1U, std::thread::hardware_concurrency()) : requested_jobs;
    for (auto offset = std::size_t{}; offset < commands.size(); offset += jobs) {
        const auto end = std::min(commands.size(), offset + jobs);
        auto tasks = std::vector<boost::capy::io_task<launch_completion>>{};
        tasks.reserve(end - offset);
        for (auto index = offset; index < end; ++index) {
            auto request = base;
            request.sandbox_root /= "seed-" + std::to_string(index);
            request.command = commands[index];
            tasks.push_back(launch_wrapped_io(std::move(request)));
        }
        auto [wait_error, results] = co_await runtime::when_all(std::move(tasks));
        if (wait_error)
            co_return std::unexpected(
                wrap_error(errc::internal, wait_error.message(), "compile_commands.json"));
        for (auto& completed : results) {
            if (!completed.value)
                co_return std::unexpected(completed.value.error());
            output.push_back(std::move(*completed.value));
        }
    }
    co_return output;
}

void add_miss(wrap_report& report, cache_miss miss) {
    ++report.cache.misses_by_reason[miss.reason];
    report.why.push_back(std::move(miss));
}

void add_timing(wrap_report& report,
                result_id result,
                const action_result& value,
                cache_status status,
                std::chrono::milliseconds start_offset = {}) {
    auto outputs = std::vector<std::string>{};
    outputs.reserve(value.outputs.size());
    for (const auto& output : value.outputs) {
        outputs.push_back(output.path);
    }
    report.trace_actions.push_back({result,
                                    value.action,
                                    std::move(outputs),
                                    value.timing,
                                    status,
                                    start_offset,
                                    value.schedule});
}

runtime::task<std::optional<result_id>>
previous_for_outputs(repository& source,
                     const std::vector<saved_action>& actions,
                     const std::set<std::string>& outputs) {
    auto selected = std::optional<result_id>{};
    auto selected_size = std::numeric_limits<std::size_t>::max();
    for (const auto& saved : actions) {
        auto value = co_await load_result(source, saved.result);
        if (!value) {
            continue;
        }
        const auto overlaps = std::ranges::any_of(value->outputs, [&](const action_output& output) {
            return outputs.contains(output.path);
        });
        if (!overlaps || value->outputs.size() >= selected_size) {
            continue;
        }
        selected = result_id{saved.result};
        selected_size = value->outputs.size();
    }
    co_return selected;
}

runtime::task<void> record_rebuilt_action(repository& source,
                                          const saved_action& saved,
                                          std::optional<result_id> previous,
                                          cache_plan& plan,
                                          bool reason_recorded,
                                          wrap_report& report,
                                          std::chrono::milliseconds start_offset = {}) {
    auto value = co_await load_result(source, saved.result);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    auto bytes = co_await build_detail::output_bytes(source, *value);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    report.cache.bytes_rebuilt += *bytes;
    add_timing(report, result_id{saved.result}, *value, cache_status::miss, start_offset);
    if (!reason_recorded) {
        auto action = co_await load_action(source, value->action);
        if (!action) {
            co_return std::unexpected(action.error());
        }
        auto cause = co_await build_detail::classify_miss(
            source, action->input, action->sandbox, previous, &*value);
        if (!cause) {
            co_return std::unexpected(cause.error());
        }
        add_miss(report, std::move(*cause));
    }
    plan.lookups.push_back({cache_status::miss, result_id{saved.result}, std::move(previous)});
    co_return liquid::result<void>{};
}

runtime::task<void> complete_report(const std::filesystem::path& cache_directory,
                                    repository& source,
                                    build_detail::disk_cache& cache,
                                    std::chrono::steady_clock::time_point started,
                                    wrap_report& report) {
    report.wall_time = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    report.peak_rss_bytes = peak_rss_bytes();
    report.cache.hits = report.hits;
    report.cache.misses = report.misses;
    report.cache.disk.hits = report.hits - report.cache.remote.hits;
    report.cache.disk.misses = report.misses + report.cache.remote.hits;
    report.cache.disk.bytes_transferred = report.cache.bytes_served;
    auto traced = co_await build_detail::record_trace(source, cache, report);
    if (!traced) {
        co_return std::unexpected(traced.error());
    }
    co_return build_detail::update_cache_stats(cache_directory, report);
}

runtime::task<void> collect_manifest(repository& source,
                                     digest root,
                                     std::string prefix,
                                     std::vector<observed_input>& output) {
    auto bytes = co_await repository_detail::build_access::get(source, root);
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    auto value = objects::decode_tree(*bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    for (const auto& entry : value->entries) {
        const auto path = prefix.empty() ? entry.name : prefix + "/" + entry.name;
        if (entry.kind == node_kind::tree) {
            auto nested = co_await collect_manifest(source, entry.ref, path, output);
            if (!nested) {
                co_return std::unexpected(nested.error());
            }
        } else if (entry.kind == node_kind::blob) {
            output.push_back({.path = path, .content = entry.ref});
        }
    }
    co_return liquid::result<void>{};
}

runtime::task<std::optional<result_id>> import_remote_result(repository& source,
                                                             build_detail::disk_cache& cache,
                                                             remote& backend,
                                                             const action_input& input,
                                                             digest sandbox,
                                                             bool eligible,
                                                             wrap_report& report) {
    report.cache.remote.available = true;
    if (!eligible) {
        co_return std::optional<result_id>{};
    }
    auto value = co_await backend.get_action_result(source, input, sandbox);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    if (!*value) {
        ++report.cache.remote.misses;
        co_return std::optional<result_id>{};
    }
    auto action_bytes = objects::encode(objects::action_record{input, sandbox});
    if (!action_bytes) {
        co_return std::unexpected(action_bytes.error());
    }
    auto action =
        co_await repository_detail::build_access::put_record(source, std::move(*action_bytes));
    if (!action) {
        co_return std::unexpected(action.error());
    }
    (*value)->action = *action;
    if ((*value)->manifest.empty()) {
        auto collected = co_await collect_manifest(source, input.inputs, {}, (*value)->manifest);
        if (!collected) {
            co_return std::unexpected(collected.error());
        }
    }
    (*value)->provenance.reset();
    (*value)->sboms.clear();
    (*value)->sig.clear();
    auto stored = co_await build_detail::store_result_with_provenance(source, **value);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto candidate = cache.candidate_key(input, sandbox);
    if (!candidate) {
        co_return std::unexpected(candidate.error());
    }
    auto published = co_await cache.publish(*candidate, *stored, **value);
    if (!published) {
        co_return std::unexpected(published.error());
    }
    auto validated = co_await cache.lookup(input, sandbox);
    if (!validated) {
        co_return std::unexpected(validated.error());
    }
    if (!validated->hit || !same_digest(validated->hit->value, stored->value)) {
        ++report.cache.remote.misses;
        co_return std::optional<result_id>{};
    }
    auto transferred = co_await build_detail::output_bytes(source, **value);
    if (!transferred) {
        co_return std::unexpected(transferred.error());
    }
    ++report.cache.remote.hits;
    report.cache.remote.bytes_transferred += *transferred;
    report.cache.remote.bytes_avoided += *transferred;
    co_return std::optional<result_id>{*stored};
}

runtime::task<cache_plan> prepare_cache_plan(repository& source,
                                             build_detail::disk_cache& cache,
                                             graph& queries,
                                             wrap_query_state& query_state,
                                             const wrap_state& state,
                                             const std::vector<std::string>& command,
                                             const std::map<std::string, std::string>& environment,
                                             digest input_tree,
                                             digest toolchain,
                                             digest sandbox,
                                             const std::filesystem::path& working_tree,
                                             const wrap_options& options,
                                             remote* backend,
                                             wrap_report& report) {
    auto output = cache_plan{};
    if (!options.cache || state.action_command != command)
        co_return output;
    auto invalidated_outputs = std::set<std::string>{};
    for (const auto& saved : state.actions) {
        auto old_result = co_await load_result(source, saved.result);
        if (!old_result)
            continue;
        auto old_action = co_await load_action(source, old_result->action);
        if (!old_action)
            continue;
        auto input = old_action->input;
        input.inputs = input_tree;
        input.toolchain = toolchain;
        input.env = environment;
        const auto reads_invalidated_output =
            std::ranges::any_of(old_result->manifest, [&](const auto& observed) {
                return invalidated_outputs.contains(observed.path);
            });
        auto lookup =
            co_await demand_wrap_action(queries, query_state, input, sandbox, *old_result);
        if (!lookup) {
            co_return std::unexpected(lookup.error());
        }
        if (lookup->hit) {
            if (reads_invalidated_output) {
                auto conditions = std::vector<cache_plan::condition>{};
                for (const auto& observed : old_result->manifest) {
                    if (!invalidated_outputs.contains(observed.path)) {
                        continue;
                    }
                    auto condition = co_await runtime::offload([working_tree, observed]() {
                        return file_condition(working_tree, observed.path);
                    });
                    if (!condition) {
                        co_return std::unexpected(condition.error());
                    }
                    if (!*condition) {
                        conditions.clear();
                        break;
                    }
                    conditions.push_back(std::move(**condition));
                }
                if (!conditions.empty()) {
                    output.conditions.emplace(saved.key, std::move(conditions));
                } else {
                    lookup->hit.reset();
                }
            }
            if (lookup->hit) {
                output.processes.insert_or_assign(saved.key, 0);
                output.next_actions.push_back({saved.key, lookup->hit->value});
                ++report.hits;
                report.cache.bytes_served += lookup->bytes_served;
                report.cache.disk.bytes_avoided += lookup->bytes_avoided;
                output.lookups.push_back(
                    {cache_status::hit, *lookup->hit, result_id{saved.result}});
                auto value = co_await load_result(source, lookup->hit->value);
                if (value) {
                    add_timing(report, *lookup->hit, *value, cache_status::hit);
                    auto materialised = co_await materialise_result(source, *value);
                    if (!materialised)
                        co_return std::unexpected(materialised.error());
                }
                continue;
            }
        }
        if (!reads_invalidated_output) {
            if (backend) {
                auto imported = co_await import_remote_result(
                    source, cache, *backend, input, sandbox, !old_result->manifest.empty(), report);
                if (!imported) {
                    co_return std::unexpected(imported.error());
                }
                if (*imported) {
                    output.processes.insert_or_assign(saved.key, 0);
                    output.next_actions.push_back({saved.key, (*imported)->value});
                    ++report.hits;
                    output.lookups.push_back(
                        {cache_status::hit, **imported, result_id{saved.result}});
                    auto value = co_await load_result(source, (*imported)->value);
                    if (!value) {
                        co_return std::unexpected(value.error());
                    }
                    add_timing(report, **imported, *value, cache_status::hit);
                    auto materialised = co_await materialise_result(source, *value);
                    if (!materialised) {
                        co_return std::unexpected(materialised.error());
                    }
                    continue;
                }
            }
            auto cause = co_await build_detail::classify_miss(
                source, input, sandbox, result_id{saved.result});
            if (!cause) {
                co_return std::unexpected(cause.error());
            }
            add_miss(report, std::move(*cause));
            output.reasoned_actions.insert(saved.key);
        }
        ++report.misses;
        output.missed_actions.insert(saved.key);
        auto invalid_outputs = std::set<std::string>{input.outputs.begin(), input.outputs.end()};
        for (const auto& action_output : old_result->outputs)
            invalid_outputs.insert(action_output.path);
        invalidated_outputs.insert(invalid_outputs.begin(), invalid_outputs.end());
        for (const auto& path : invalid_outputs) {
            auto removed = co_await source.remove(path);
            if (!removed && removed.error().code != errc::not_found)
                co_return std::unexpected(removed.error());
        }
    }
    co_return output;
}

runtime::task<bool> serve_outer_action(repository& source,
                                       build_detail::disk_cache& cache,
                                       graph& queries,
                                       wrap_query_state& query_state,
                                       wrap_state& state,
                                       const std::filesystem::path& state_path,
                                       const std::filesystem::path& working_tree,
                                       const std::vector<std::string>& command,
                                       const action_input& input,
                                       digest sandbox,
                                       const wrap_options& options,
                                       remote* backend,
                                       wrap_report& report) {
    if (!options.cache)
        co_return false;
    auto lookup = build_detail::cache_lookup{};
    if (state.outer_result) {
        auto previous = co_await load_result(source, *state.outer_result);
        if (previous) {
            auto demanded =
                co_await demand_wrap_action(queries, query_state, input, sandbox, *previous);
            if (!demanded) {
                co_return std::unexpected(demanded.error());
            }
            lookup = std::move(*demanded);
        } else {
            auto direct = co_await cache.lookup(input, sandbox);
            if (!direct) {
                co_return std::unexpected(direct.error());
            }
            lookup = std::move(*direct);
        }
    } else {
        auto direct = co_await cache.lookup(input, sandbox);
        if (!direct) {
            co_return std::unexpected(direct.error());
        }
        lookup = std::move(*direct);
    }
    if (!lookup.hit) {
        if (backend) {
            auto previous = state.outer_result
                                ? std::optional<result_id>{result_id{*state.outer_result}}
                                : std::nullopt;
            auto eligible = false;
            if (previous) {
                auto previous_value = co_await load_result(source, previous->value);
                eligible = previous_value && !previous_value->manifest.empty();
            }
            auto imported = co_await import_remote_result(
                source, cache, *backend, input, sandbox, eligible, report);
            if (!imported) {
                co_return std::unexpected(imported.error());
            }
            if (*imported) {
                auto direct = co_await cache.lookup(input, sandbox);
                if (!direct) {
                    co_return std::unexpected(direct.error());
                }
                lookup = std::move(*direct);
            }
        }
    }
    if (!lookup.hit) {
        ++report.misses;
        auto previous = state.outer_result
                            ? std::optional<result_id>{result_id{*state.outer_result}}
                            : std::nullopt;
        auto cause = co_await build_detail::classify_miss(source, input, sandbox, previous);
        if (!cause) {
            co_return std::unexpected(cause.error());
        }
        add_miss(report, std::move(*cause));
        co_return false;
    }
    auto value = co_await load_result(source, lookup.hit->value);
    if (!value)
        co_return std::unexpected(value.error());
    auto materialised = co_await materialise_result(source, *value, working_tree);
    if (!materialised)
        co_return std::unexpected(materialised.error());
    for (const auto& action_output : value->outputs)
        state.outputs.insert(action_output.path);
    report.exit = value->exit;
    report.outer = cache_status::hit;
    report.actions = (state.action_command == command ? state.actions.size() : std::size_t{}) + 1;
    report.hits = report.actions;
    report.cache.bytes_served += lookup.bytes_served;
    report.cache.disk.bytes_avoided += lookup.bytes_avoided;
    auto recorded = cache.record_lookup(
        cache_status::hit,
        *lookup.hit,
        state.outer_result ? std::optional<result_id>{result_id{*state.outer_result}}
                           : std::nullopt);
    if (!recorded) {
        co_return std::unexpected(recorded.error());
    }
    for (const auto& saved_action : state.actions) {
        auto child = co_await load_result(source, saved_action.result);
        if (!child) {
            continue;
        }
        const auto already_traced =
            std::ranges::any_of(report.trace_actions, [&](const auto& item) {
                return same_digest(item.result.value, saved_action.result);
            });
        if (!already_traced) {
            add_timing(report, result_id{saved_action.result}, *child, cache_status::hit);
        }
        auto child_recorded = cache.record_lookup(
            cache_status::hit, result_id{saved_action.result}, result_id{saved_action.result});
        if (!child_recorded) {
            co_return std::unexpected(child_recorded.error());
        }
    }
    if (report.trace_actions.empty()) {
        add_timing(report, *lookup.hit, *value, cache_status::hit);
    }
    auto saved = write_state(state_path, state);
    if (!saved)
        co_return std::unexpected(saved.error());
    co_return true;
}

runtime::task<observed_run>
execute_observed_run(repository& source,
                     const std::filesystem::path& cache_directory,
                     const std::filesystem::path& working_tree,
                     const std::vector<std::string>& command,
                     const std::set<std::string>& known_outputs,
                     const std::map<std::uint64_t, int>& plan,
                     const std::map<std::uint64_t, std::vector<cache_plan::condition>>& conditions,
                     const std::map<std::string, std::string>& action_environment,
                     digest input_tree,
                     const wrap_options& options) {
    static std::atomic_uint64_t sequence{};
    const auto run = cache_directory / "runs" /
                     (std::to_string(static_cast<std::uint64_t>(::getpid())) + "-" +
                      std::to_string(sequence.fetch_add(1)));
    const auto mount = run / "mount";
    const auto root = run / "root";
    const auto trace = run / "trace";
    const auto plan_path = run / "plan";
    auto code = std::error_code{};
    std::filesystem::create_directories(mount, code);
    std::filesystem::create_directories(root, code);
    if (code)
        co_return std::unexpected(wrap_error(errc::io, code.message(), run));
    {
        auto trace_output = std::ofstream{trace, std::ios::trunc};
        auto plan_output = std::ofstream{plan_path, std::ios::trunc};
        for (const auto& [key, status] : plan) {
            plan_output << "H\t" << std::hex << key << '\t' << std::dec << status << '\n';
            const auto found = conditions.find(key);
            if (found == conditions.end()) {
                continue;
            }
            for (const auto& condition : found->second) {
                plan_output << "C\t" << std::hex << key << '\t' << condition.fingerprint << '\t'
                            << std::dec << condition.size << '\t' << hex(condition.path) << '\n';
            }
        }
        if (!trace_output || !plan_output)
            co_return std::unexpected(wrap_error(errc::io, "cannot prepare wrap hook files", run));
    }
    auto observations = std::make_shared<observation_log>();
    auto mounted = co_await vfs_detail::start_observed_fuse(
        source,
        input_tree,
        mount,
        {known_outputs.begin(), known_outputs.end()},
        [observations](vfs_detail::observed_request request) {
            auto lock = std::scoped_lock{observations->mutex};
            observations->requests.push_back(std::move(request));
        },
        false);
    if (!mounted) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(mounted.error());
    }
    auto environment = action_environment;
    environment["LIQUID_WRAP_ACTIVE"] = "1";
    auto hook = wrap_hook_path();
    if (!hook) {
        static_cast<void>(co_await (*mounted)->unmount());
        static_cast<void>(remove_run(run));
        co_return std::unexpected(hook.error());
    }
#if defined(__APPLE__)
    environment["DYLD_INSERT_LIBRARIES"] = hook->string();
    environment["LIQUID_WRAP_PLAN"] = plan_path.string();
    environment["LIQUID_WRAP_ROOT"] = mount.string();
    environment["LIQUID_WRAP_TRACE"] = trace.string();
#else
    environment["LD_PRELOAD"] = "/liquid/wrap-hook.so";
    environment["LIQUID_WRAP_PLAN"] = "/liquid/plan";
    environment["LIQUID_WRAP_ROOT"] = "/liquid/work";
    environment["LIQUID_WRAP_TRACE"] = "/liquid/trace";
#endif
    auto commands = planned_commands(working_tree, command);
    if (!commands) {
        static_cast<void>(co_await (*mounted)->unmount());
        static_cast<void>(remove_run(run));
        co_return std::unexpected(commands.error());
    }
    auto request = sandbox_detail::wrap_launch_request{.sandbox_root = root,
                                                       .input_mount = mount,
                                                       .hook = *hook,
                                                       .trace = trace,
                                                       .plan = plan_path,
                                                       .command = {},
                                                       .environment = environment};
    const auto started_at =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto seeded = co_await launch_seed_processes(request, commands->seeds, options.jobs);
    if (!seeded) {
        static_cast<void>(co_await (*mounted)->unmount());
        static_cast<void>(remove_run(run));
        co_return std::unexpected(seeded.error());
    }
    auto launch = sandbox_detail::launch_result{};
    for (const auto& seed : *seeded) {
        if (seed.exit != 0 && launch.exit == 0)
            launch = seed;
    }
    if (launch.exit == 0 && !commands->driver.empty()) {
        request.sandbox_root /= "driver";
        request.command = std::move(commands->driver);
        auto launched = co_await launch_wrapped_process(std::move(request));
        if (!launched) {
            static_cast<void>(co_await (*mounted)->unmount());
            static_cast<void>(remove_run(run));
            co_return std::unexpected(launched.error());
        }
        launch = *launched;
    }
    auto unmounted = co_await (*mounted)->unmount();
    if (!unmounted) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(unmounted.error());
    }
    auto processes = parse_trace(trace);
    if (!processes) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(processes.error());
    }
    if (processes->empty()) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(wrap_error(
            errc::unsupported, "wrapped driver did not load the exec-time hook", command.front()));
    }
    const auto finished_at =
        std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto trace_origin = std::uint64_t{};
    for (const auto& process : *processes) {
        if (process.started_ns != 0 && (trace_origin == 0 || process.started_ns < trace_origin)) {
            trace_origin = process.started_ns;
        }
    }
    co_return observed_run{run,
                           std::move(observations),
                           std::move(*processes),
                           launch,
                           started_at,
                           finished_at,
                           trace_origin};
}

runtime::task<void> publish_observed_run(repository& source,
                                         build_detail::disk_cache& cache,
                                         build_detail::fact_base& facts,
                                         const fact_key& facts_key,
                                         wrap_state& state,
                                         const std::filesystem::path& state_path,
                                         const std::filesystem::path& toolchain_path,
                                         const std::filesystem::path& working_tree,
                                         const std::vector<std::string>& command,
                                         const std::map<std::string, std::string>& environment,
                                         digest sandbox,
                                         std::optional<digest>& toolchain,
                                         cache_plan& plan,
                                         observed_run& run,
                                         const wrap_options& options,
                                         remote* backend,
                                         bool fact_answers_used,
                                         bool outer_miss_recorded,
                                         wrap_report& report) {
    report.exit = run.launch.exit;
    auto cut = co_await source.cut("wrap output snapshot");
    if (!cut) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(cut.error());
    }
    auto log = co_await source.log();
    if (!log || log->empty()) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(log ? wrap_error(errc::internal, "wrap output is missing")
                                      : log.error());
    }
    const auto replay_tree = log->front().root;
    const auto previous_actions = state.actions;
    const auto previous_outer = state.outer_result
                                    ? std::optional<result_id>{result_id{*state.outer_result}}
                                    : std::nullopt;
    if (!toolchain) {
        for (const auto& process : run.processes) {
            state.tools.insert(process.executable);
        }
        auto stored = co_await store_toolchain(source, state.tools);
        if (!stored) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(stored.error());
        }
        toolchain = *stored;
        auto pinned = write_toolchain(toolchain_path, *stored);
        if (!pinned) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(pinned.error());
        }
    }
    const auto empty = run.directory / "empty";
    {
        auto output = std::ofstream{empty, std::ios::trunc};
    }
    auto empty_blob = co_await repository_detail::build_access::put_file(source, empty);
    if (!empty_blob) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(empty_blob.error());
    }
    const auto writes = writes_by_process(*run.observations);
    const auto driver_pid = run.processes.front().pid;
    auto all_writes = std::set<std::string>{};
    for (const auto& [pid, paths] : writes)
        all_writes.insert(paths.begin(), paths.end());
    for (const auto& process : run.processes) {
        auto program = co_await probe_program_text(source, *run.observations, process.pid);
        if (!program) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(program.error());
        }
        const auto is_probe = build_detail::probe_process(process.command, process.cwd) &&
                              shareable_probe_program(*program);
        if (is_probe && !fact_answers_used && !plan.processes.contains(process.key)) {
            ++report.probes_started;
            auto answer = fact_answer{};
            const auto found = writes.find(process.pid);
            answer.exit = found == writes.end() || found->second.empty() ? 1 : 0;
            if (found != writes.end()) {
                auto outputs = co_await collect_action_outputs(source, found->second);
                if (!outputs) {
                    static_cast<void>(remove_run(run.directory));
                    co_return std::unexpected(outputs.error());
                }
                answer.outputs = std::move(*outputs);
            }
            auto canonical = build_detail::canonical_probe(process.command, process.cwd, *program);
            if (answer.outputs.empty()) {
                auto recorded = facts.put(std::move(canonical), facts_key, std::move(answer));
                if (!recorded) {
                    static_cast<void>(remove_run(run.directory));
                    co_return std::unexpected(recorded.error());
                }
            }
        }
        const auto found = writes.find(process.pid);
        const auto conditional_miss = plan.conditions.contains(process.key) &&
                                      found != writes.end() && !found->second.empty();
        if (process.pid == driver_pid ||
            (plan.processes.contains(process.key) && !conditional_miss) ||
            !state.tools.contains(process.executable) ||
            has_writing_descendant(process, run.processes, writes)) {
            continue;
        }
        if (found == writes.end() || found->second.empty())
            continue;
        if (conditional_miss) {
            if (report.hits != 0) {
                --report.hits;
            }
            plan.next_actions.erase(
                std::remove_if(plan.next_actions.begin(),
                               plan.next_actions.end(),
                               [&](const saved_action& item) { return item.key == process.key; }),
                plan.next_actions.end());
            plan.missed_actions.insert(process.key);
        }
        auto previous = co_await previous_for_outputs(source, previous_actions, found->second);
        if (!previous) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(previous.error());
        }
        auto published = co_await publish_action(source,
                                                 cache,
                                                 process,
                                                 environment,
                                                 replay_tree,
                                                 *toolchain,
                                                 sandbox,
                                                 *run.observations,
                                                 found->second,
                                                 *empty_blob,
                                                 0,
                                                 run.started_at,
                                                 run.finished_at,
                                                 options.cache);
        if (!published) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(published.error());
        }
        if (is_probe && !fact_answers_used) {
            auto answer = fact_answer{.exit = 0, .result = published->result};
            auto outputs = co_await collect_action_outputs(source, found->second);
            if (!outputs) {
                static_cast<void>(remove_run(run.directory));
                co_return std::unexpected(outputs.error());
            }
            answer.outputs = std::move(*outputs);
            auto canonical = build_detail::canonical_probe(process.command, process.cwd, *program);
            auto recorded = facts.put(std::move(canonical), facts_key, std::move(answer));
            if (!recorded) {
                static_cast<void>(remove_run(run.directory));
                co_return std::unexpected(recorded.error());
            }
        }
        plan.next_actions.push_back(*published);
        auto recorded = co_await record_rebuilt_action(
            source,
            *published,
            *previous,
            plan,
            plan.reasoned_actions.contains(process.key),
            report,
            process.started_ns >= run.trace_origin_ns
                ? std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::nanoseconds{process.started_ns - run.trace_origin_ns})
                : std::chrono::milliseconds{});
        if (!recorded) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(recorded.error());
        }
        ++report.processes_started;
        if (!plan.missed_actions.contains(process.key))
            ++report.misses;
    }
    auto driver = run.processes.front();
    driver.pid = -1;
    driver.command = command;
    driver.cwd = std::string{virtual_root};
    auto outer_saved = co_await publish_action(source,
                                               cache,
                                               driver,
                                               environment,
                                               replay_tree,
                                               *toolchain,
                                               sandbox,
                                               *run.observations,
                                               all_writes,
                                               *empty_blob,
                                               report.exit,
                                               run.started_at,
                                               run.finished_at,
                                               options.cache);
    if (!outer_saved) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(outer_saved.error());
    }
    auto outer_value = co_await load_result(source, outer_saved->result);
    if (!outer_value) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(outer_value.error());
    }
    if (backend && backend->config().publish) {
        auto outer_action = co_await load_action(source, outer_value->action);
        if (!outer_action) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(outer_action.error());
        }
        auto uploaded = co_await backend->update_action_result(
            source, outer_action->input, outer_action->sandbox, *outer_value);
        if (!uploaded) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(uploaded.error());
        }
    }
    auto outer_bytes = co_await build_detail::output_bytes(source, *outer_value);
    if (!outer_bytes) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(outer_bytes.error());
    }
    report.cache.bytes_rebuilt += *outer_bytes;
    if (report.trace_actions.empty()) {
        add_timing(report, result_id{outer_saved->result}, *outer_value, cache_status::miss);
    }
    if (!outer_miss_recorded) {
        ++report.misses;
        auto outer_action = co_await load_action(source, outer_value->action);
        if (!outer_action) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(outer_action.error());
        }
        auto cause = co_await build_detail::classify_miss(
            source, outer_action->input, outer_action->sandbox, previous_outer);
        if (!cause) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(cause.error());
        }
        add_miss(report, std::move(*cause));
    }
    auto previous_order = std::map<std::uint64_t, std::size_t>{};
    for (std::size_t index = 0; index < previous_actions.size(); ++index) {
        previous_order.emplace(previous_actions[index].key, index);
    }
    std::ranges::stable_sort(
        plan.next_actions, [&](const saved_action& left, const saved_action& right) {
            const auto left_order = previous_order.find(left.key);
            const auto right_order = previous_order.find(right.key);
            const auto left_index = left_order == previous_order.end()
                                        ? std::numeric_limits<std::size_t>::max()
                                        : left_order->second;
            const auto right_index = right_order == previous_order.end()
                                         ? std::numeric_limits<std::size_t>::max()
                                         : right_order->second;
            return left_index < right_index;
        });
    state.actions = std::move(plan.next_actions);
    state.outer_result = outer_saved->result;
    state.action_command = command;
    state.outputs.insert(all_writes.begin(), all_writes.end());
    auto copied = co_await copy_outputs(source, working_tree, state.outputs);
    if (!copied) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(copied.error());
    }
    auto saved = write_state(state_path, state);
    auto removed = remove_run(run.directory);
    if (!saved)
        co_return std::unexpected(saved.error());
    if (!removed)
        co_return std::unexpected(removed.error());
    auto outer_recorded =
        cache.record_lookup(cache_status::miss, result_id{outer_saved->result}, previous_outer);
    if (!outer_recorded)
        co_return std::unexpected(outer_recorded.error());
    for (const auto& lookup : plan.lookups) {
        auto recorded = cache.record_lookup(lookup.status, lookup.result, lookup.previous);
        if (!recorded)
            co_return std::unexpected(recorded.error());
    }
    report.actions = state.actions.size() + 1;
    co_return liquid::result<void>{};
}

} // namespace

objects::sandbox_record wrap_sandbox_definition() {
    return {.mechanism = "wrap user+mount+net namespaces with exec-time hook v1",
            .work_root = "/liquid/work/",
            .toolchain_root = "/usr/",
            .home_root = "/tmp/home/",
            .temporary_root = "/tmp/",
            .fixed_environment = {"PATH", "HOME", "TMPDIR", "LANG", "TZ", "SOURCE_DATE_EPOCH"},
            .network = false};
}

liquid::result<std::filesystem::path>
wrap_cache_directory(const std::filesystem::path& working_tree) {
    return default_cache_directory(working_tree);
}

runtime::task<action_result> run_wrapped_action(repository& source,
                                                const objects::action_record& action,
                                                const action_result& expected,
                                                std::chrono::sys_seconds observed_at) {
    static std::atomic_uint64_t verification_sequence{};
    auto repository_root = repository_detail::build_access::root_path(source);
    if (!repository_root) {
        co_return std::unexpected(repository_root.error());
    }
    const auto run = *repository_root / ".liquid-build" / "verify" /
                     std::to_string(verification_sequence.fetch_add(1));
    const auto mount = run / "mount";
    const auto sandbox_root = run / "root";
    const auto trace = run / "trace";
    const auto plan = run / "plan";
    const auto stdout_path = run / "stdout";
    const auto stderr_path = run / "stderr";
    auto code = std::error_code{};
    std::filesystem::create_directories(run, code);
    if (code) {
        co_return std::unexpected(wrap_error(errc::io, code.message(), run));
    }
    auto materialised = co_await build_detail::materialise_tree(source, action.input.inputs, mount);
    if (!materialised) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(materialised.error());
    }
    for (const auto& output : expected.outputs) {
        std::filesystem::remove_all(mount / output.path, code);
        if (code) {
            static_cast<void>(remove_run(run));
            co_return std::unexpected(wrap_error(errc::io, code.message(), mount / output.path));
        }
    }
    {
        auto trace_output = std::ofstream{trace, std::ios::trunc};
        auto plan_output = std::ofstream{plan, std::ios::trunc};
        if (!trace_output || !plan_output) {
            static_cast<void>(remove_run(run));
            co_return std::unexpected(
                wrap_error(errc::io, "cannot prepare verification hook files", run));
        }
    }
    auto hook = wrap_hook_path();
    if (!hook) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(hook.error());
    }
    auto environment = action.input.env;
#if defined(__linux__)
    environment["LD_PRELOAD"] = "/liquid/wrap-hook.so";
    environment["LIQUID_WRAP_TRACE"] = "/liquid/trace";
    environment["LIQUID_WRAP_PLAN"] = "/liquid/plan";
#elif defined(__APPLE__)
    environment["DYLD_INSERT_LIBRARIES"] = hook->string();
    environment["LIQUID_WRAP_ROOT"] = mount.string();
    environment["LIQUID_WRAP_TRACE"] = trace.string();
    environment["LIQUID_WRAP_PLAN"] = plan.string();
#endif
    environment["LIQUID_WRAP_ACTIVE"] = "1";
    const auto cwd =
        action.input.cwd.empty() ? std::string{"/liquid/work"} : "/liquid/work/" + action.input.cwd;
    auto request = sandbox_detail::wrap_launch_request{.sandbox_root = sandbox_root,
                                                       .input_mount = mount,
                                                       .hook = *hook,
                                                       .trace = trace,
                                                       .plan = plan,
                                                       .stdout_path = stdout_path,
                                                       .stderr_path = stderr_path,
                                                       .cwd = cwd,
                                                       .command = action.input.command,
                                                       .environment = std::move(environment)};
    build_detail::testing::record_sandbox_spawn();
    auto launched = co_await runtime::offload(
        [request = std::move(request)] { return sandbox_detail::launch_wrap(request); });
    if (!launched || !*launched) {
        static_cast<void>(remove_run(run));
        if (!launched) {
            co_return std::unexpected(launched.error());
        }
        co_return std::unexpected(launched->error());
    }
    auto output_values = std::vector<action_output>{};
    for (const auto& declared : expected.outputs) {
        const auto relative = declared.path;
        const auto path = mount / relative;
        if (!std::filesystem::exists(path, code)) {
            continue;
        }
        const auto directory = std::filesystem::is_directory(path, code);
        if (code) {
            static_cast<void>(remove_run(run));
            co_return std::unexpected(wrap_error(errc::io, code.message(), path));
        }
        auto files = std::vector<std::filesystem::path>{};
        if (directory) {
            for (auto iterator = std::filesystem::recursive_directory_iterator{path, code};
                 !code && iterator != std::filesystem::recursive_directory_iterator{};
                 ++iterator) {
                if (iterator->is_regular_file()) {
                    files.push_back(iterator->path());
                }
            }
        } else {
            files.push_back(path);
        }
        if (code) {
            static_cast<void>(remove_run(run));
            co_return std::unexpected(wrap_error(errc::io, code.message(), path));
        }
        for (const auto& file : files) {
            auto stored = co_await repository_detail::build_access::put_file(source, file);
            if (!stored) {
                static_cast<void>(remove_run(run));
                co_return std::unexpected(stored.error());
            }
            const auto status = std::filesystem::status(file, code);
            if (code) {
                static_cast<void>(remove_run(run));
                co_return std::unexpected(wrap_error(errc::io, code.message(), file));
            }
            output_values.push_back({std::filesystem::relative(file, mount).generic_string(),
                                     static_cast<std::uint32_t>(status.permissions()) & 0777U,
                                     *stored});
        }
    }
    std::ranges::sort(output_values, {}, &action_output::path);
    auto stdout_id = co_await repository_detail::build_access::put_file(source, stdout_path);
    auto stderr_id = co_await repository_detail::build_access::put_file(source, stderr_path);
    if (!stdout_id || !stderr_id) {
        static_cast<void>(remove_run(run));
        co_return std::unexpected(!stdout_id ? stdout_id.error() : stderr_id.error());
    }
    auto result =
        action_result{.action = expected.action,
                      .exit = (*launched)->exit,
                      .outputs = std::move(output_values),
                      .stdout_output = *stdout_id,
                      .stderr_output = *stderr_id,
                      .worker = action.input.platform.os + "/" + action.input.platform.arch,
                      .observed_at = observed_at};
    auto signed_result = co_await repository_detail::build_access::sign_result(source, result);
    auto removed = remove_run(run);
    if (!signed_result) {
        co_return std::unexpected(signed_result.error());
    }
    if (!removed) {
        co_return std::unexpected(removed.error());
    }
    co_return result;
}

runtime::task<wrap_report>
wrap(std::filesystem::path working_tree, std::vector<std::string> command, wrap_options options) {
#if defined(_WIN32)
    co_return std::unexpected(
        wrap_error(errc::unsupported, "liquid wrap is unavailable on Windows", "liquid Phase 6"));
#else
    const auto wrap_started = std::chrono::steady_clock::now();
    if (command.empty() || command.front().empty())
        co_return std::unexpected(wrap_error(errc::invalid_argument, "wrap command is empty"));
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree))
        co_return std::unexpected(wrap_error(
            errc::invalid_argument, "wrap working tree is not a directory", working_tree));
    auto fact_directory = options.fact_directory
                              ? liquid::result<std::filesystem::path>{*options.fact_directory}
                              : build_detail::default_fact_directory(working_tree);
    if (!fact_directory)
        co_return std::unexpected(fact_directory.error());
    auto facts = build_detail::fact_base{*fact_directory};
    auto facts_opened = facts.open();
    if (!facts_opened)
        co_return std::unexpected(facts_opened.error());
    auto facts_key = build_detail::detect_fact_key(options.target);
    if (!facts_key)
        co_return std::unexpected(facts_key.error());
    auto action_environment = fixed_environment(options);
    const auto fact_variables = build_detail::cached_variables(facts, *facts_key);
    apply_cached_variables(command, action_environment, fact_variables);
    if (build_detail::foreign_target(*facts_key))
        action_environment["LIQUID_WRAP_REFUSE_PROBES"] = "1";
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : wrap_cache_directory(working_tree);
    if (!cache_directory)
        co_return std::unexpected(cache_directory.error());
    std::filesystem::create_directories(*cache_directory, code);
    if (code)
        co_return std::unexpected(wrap_error(errc::io, code.message(), *cache_directory));
    const auto state_path = *cache_directory / "state";
    const auto toolchain_path = *cache_directory / "config.wrap.toolchain";
    auto state = read_state(state_path);
    if (!state)
        co_return std::unexpected(state.error());

    const auto repository_path = *cache_directory / "repository";
    auto source =
        std::filesystem::exists(repository_path / "LIQUID_REPO")
            ? co_await repository::open(repository_path, {.defer_tree_load = true})
            : co_await repository::create(repository_path,
                                          repository_options{.cache = cache_config{.publish = true},
                                                             .remote = options.remote});
    if (!source)
        co_return std::unexpected(source.error());
    auto remote_configuration = options.remote ? options.remote : source->remote_configuration();
    auto backend = std::optional<remote>{};
    if (remote_configuration) {
        auto connected = co_await remote::connect(*remote_configuration);
        if (!connected) {
            co_return std::unexpected(connected.error());
        }
        backend = std::move(*connected);
    }
    auto cache = build_detail::disk_cache{*source};
    auto opened = co_await cache.open();
    if (!opened)
        co_return std::unexpected(opened.error());
    auto query_state = wrap_query_state{&*source, &cache, {}};
    auto query_kinds = query_registry{};
    auto query_registered = query_kinds.add(
        {.name = "action_result",
         .compute = [&query_state](query_context& context, std::vector<std::byte> arguments) {
             return wrap_action_result_query(context, std::move(arguments), &query_state);
         }});
    if (!query_registered) {
        co_return std::unexpected(query_registered.error());
    }
    auto queries = co_await graph::open(
        *cache_directory / "query-graph", std::move(query_kinds), {.working_tree = working_tree});
    if (!queries) {
        co_return std::unexpected(queries.error());
    }
    auto synced = co_await sync_sources(*source, working_tree, *cache_directory, *state, command);
    if (!synced)
        co_return std::unexpected(synced.error());
    auto cut = co_await source->cut("wrap input snapshot");
    if (!cut)
        co_return std::unexpected(cut.error());
    auto log = co_await source->log();
    if (!log || log->empty())
        co_return std::unexpected(log ? wrap_error(errc::internal, "wrap snapshot is missing")
                                      : log.error());
    const auto input_tree = log->front().root;
    auto toolchain = read_toolchain(toolchain_path);
    if (!toolchain)
        co_return std::unexpected(toolchain.error());
    if (*toolchain && !state->tools.empty()) {
        auto refreshed = co_await store_toolchain(*source, state->tools);
        if (!refreshed)
            co_return std::unexpected(refreshed.error());
        if (!same_digest(**toolchain, *refreshed)) {
            *toolchain = *refreshed;
            auto pinned = write_toolchain(toolchain_path, *refreshed);
            if (!pinned)
                co_return std::unexpected(pinned.error());
        }
    }
    auto sandbox = co_await store_sandbox(*source);
    if (!sandbox)
        co_return std::unexpected(sandbox.error());
    auto report = wrap_report{};
    auto outer_miss_recorded = false;
    auto outer_input = action_input{.command = command,
                                    .cwd = "",
                                    .env = action_environment,
                                    .outputs = {},
                                    .inputs = input_tree,
                                    .toolchain = *toolchain ? **toolchain : digest{},
                                    .platform = host_platform(),
                                    .timeout = {}};
    auto policy = co_await build_detail::enforce_policy(*source, outer_input);
    if (!policy)
        co_return std::unexpected(policy.error());
    report.policy_warnings = policy->warnings;
    if (!policy->allowed)
        co_return std::unexpected(build_detail::policy_refusal(*policy));
    auto plan = cache_plan{};
    if (options.cache && *toolchain) {
        auto served = co_await serve_outer_action(*source,
                                                  cache,
                                                  *queries,
                                                  query_state,
                                                  *state,
                                                  state_path,
                                                  working_tree,
                                                  command,
                                                  outer_input,
                                                  *sandbox,
                                                  options,
                                                  backend ? &*backend : nullptr,
                                                  report);
        if (!served)
            co_return std::unexpected(served.error());
        if (*served) {
            auto completed =
                co_await complete_report(*cache_directory, *source, cache, wrap_started, report);
            if (!completed) {
                co_return std::unexpected(completed.error());
            }
            if (options.verify) {
                build_detail::schedule_verification(std::move(*source));
                report.verification_scheduled = true;
            }
            co_return report;
        }
        outer_miss_recorded = true;
    }
    for (const auto& saved : state->actions) {
        auto value = co_await load_result(*source, saved.result);
        if (!value) {
            continue;
        }
        auto materialised = co_await materialise_result(*source, *value);
        if (!materialised) {
            co_return std::unexpected(materialised.error());
        }
    }
    if (*toolchain) {
        auto prepared = co_await prepare_cache_plan(*source,
                                                    cache,
                                                    *queries,
                                                    query_state,
                                                    *state,
                                                    command,
                                                    action_environment,
                                                    input_tree,
                                                    **toolchain,
                                                    *sandbox,
                                                    working_tree,
                                                    options,
                                                    backend ? &*backend : nullptr,
                                                    report);
        if (!prepared)
            co_return std::unexpected(prepared.error());
        plan = std::move(*prepared);
    }

    auto run = co_await execute_observed_run(*source,
                                             *cache_directory,
                                             working_tree,
                                             command,
                                             state->outputs,
                                             plan.processes,
                                             plan.conditions,
                                             action_environment,
                                             input_tree,
                                             options);
    if (!run)
        co_return std::unexpected(run.error());
    if (build_detail::foreign_target(*facts_key) &&
        std::ranges::any_of(run->processes, [](const traced_process& process) {
            return build_detail::probe_process(process.command, process.cwd);
        })) {
        static_cast<void>(remove_run(run->directory));
        co_return std::unexpected(wrap_error(errc::unsupported,
                                             "configure fact missing; foreign probe was refused",
                                             facts_key->target));
    }
    auto published = co_await publish_observed_run(*source,
                                                   cache,
                                                   facts,
                                                   *facts_key,
                                                   *state,
                                                   state_path,
                                                   toolchain_path,
                                                   working_tree,
                                                   command,
                                                   action_environment,
                                                   *sandbox,
                                                   *toolchain,
                                                   plan,
                                                   *run,
                                                   options,
                                                   backend ? &*backend : nullptr,
                                                   !fact_variables.empty(),
                                                   outer_miss_recorded,
                                                   report);
    if (!published)
        co_return std::unexpected(published.error());
    auto primed = co_await prime_wrap_actions(*queries, query_state, *state, *sandbox);
    if (!primed) {
        co_return std::unexpected(primed.error());
    }
    auto recorded = build_detail::record_configuration_facts(working_tree, *facts_key, facts);
    if (!recorded)
        co_return std::unexpected(recorded.error());
    auto completed =
        co_await complete_report(*cache_directory, *source, cache, wrap_started, report);
    if (!completed) {
        co_return std::unexpected(completed.error());
    }
    if (options.verify) {
        build_detail::schedule_verification(std::move(*source));
        report.verification_scheduled = true;
    }
    co_return report;
#endif
}

} // namespace liquid::build
