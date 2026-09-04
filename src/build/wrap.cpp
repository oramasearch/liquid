#include "liquid/build.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <set>
#include <sstream>
#include <sys/stat.h>
#include <thread>

#if !defined(_WIN32)
#include <unistd.h>
#endif
#if defined(__APPLE__)
#include <mach-o/dyld.h>
#endif

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "formats/json.hpp"
#include "objects/records.hpp"
#include "sandbox/sandbox.hpp"
#include "vfs/backend.hpp"
#include "vfs/repository_access.hpp"

#ifndef LIQUID_WRAP_HOOK_PATH
#define LIQUID_WRAP_HOOK_PATH ""
#endif
#ifndef LIQUID_WRAP_HOOK_FILENAME
#define LIQUID_WRAP_HOOK_FILENAME "libliquid-wrap-hook.so"
#endif

namespace liquid::build {
namespace {

constexpr auto virtual_root = std::string_view{"/liquid/work"};

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
    std::set<std::string> sources;
    std::set<std::string> outputs;
    std::set<std::string> tools;
    std::vector<std::string> action_command;
    std::vector<saved_action> actions;
};

struct cache_plan {
    std::vector<saved_action> next_actions;
    std::set<std::uint64_t> missed_actions;
    std::map<std::uint64_t, int> processes;
};

struct observed_run {
    std::filesystem::path directory;
    std::shared_ptr<observation_log> observations;
    std::vector<traced_process> processes;
    sandbox_detail::launch_result launch;
};

auto host_platform() -> platform;

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
        auto value = unhex(fields[1]);
        if (!value)
            return std::unexpected(value.error());
        if (fields[0] == "S")
            output.sources.insert(std::move(*value));
        else if (fields[0] == "O")
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
    for (const auto& source : state.sources)
        output << "S\t" << hex(source) << '\n';
    for (const auto& item : state.outputs)
        output << "O\t" << hex(item) << '\n';
    for (const auto& tool : state.tools)
        output << "T\t" << hex(tool) << '\n';
    for (const auto& argument : state.action_command)
        output << "C\t" << hex(argument) << '\n';
    for (const auto& action : state.actions)
        output << "A\t" << std::hex << action.key << '\t' << to_string(action.result) << '\n';
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

auto project_key(const std::filesystem::path& root) -> liquid::result<std::string> {
    auto material = std::string{};
    for (const auto* name :
         {"CMakeLists.txt", "configure.ac", "configure.in", "Makefile", "compile_commands.json"}) {
        const auto path = root / name;
        if (!std::filesystem::is_regular_file(path))
            continue;
        auto input = std::ifstream{path, std::ios::binary};
        if (!input)
            return std::unexpected(wrap_error(errc::io, "cannot identify wrapped project", path));
        material += name;
        material.push_back('\0');
        material.append(std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{});
        material.push_back('\0');
    }
    if (material.empty())
        material = root.filename().string();
    auto value = hash(algorithm::blake3,
                      {reinterpret_cast<const std::byte*>(material.data()), material.size()});
    if (!value)
        return std::unexpected(value.error());
    auto text = to_string(*value);
    std::ranges::replace(text, ':', '-');
    return text;
}

auto default_cache_directory(const std::filesystem::path& root)
    -> liquid::result<std::filesystem::path> {
    auto key = project_key(root);
    if (!key)
        return std::unexpected(key.error());
    if (const auto* configured = std::getenv("LIQUID_WRAP_CACHE"))
        return std::filesystem::path{configured} / *key;
    if (const auto* configured = std::getenv("XDG_CACHE_HOME"))
        return std::filesystem::path{configured} / "liquid/wrap" / *key;
    if (const auto* home = std::getenv("HOME"))
        return std::filesystem::path{home} / ".cache/liquid/wrap" / *key;
    return root / ".liquid-wrap-cache" / *key;
}

auto wrap_hook_path() -> liquid::result<std::filesystem::path> {
    auto candidates = std::vector<std::filesystem::path>{};
    if (const auto* configured = std::getenv("LIQUID_WRAP_HOOK"))
        candidates.emplace_back(configured);
    candidates.emplace_back(LIQUID_WRAP_HOOK_PATH);
#if defined(__linux__)
    auto code = std::error_code{};
    const auto executable = std::filesystem::read_symlink("/proc/self/exe", code);
    if (!code)
        candidates.push_back(executable.parent_path() / "../libexec/liquid" /
                             LIQUID_WRAP_HOOK_FILENAME);
#elif defined(__APPLE__)
    auto size = std::uint32_t{};
    static_cast<void>(_NSGetExecutablePath(nullptr, &size));
    auto executable = std::string(size, '\0');
    if (_NSGetExecutablePath(executable.data(), &size) == 0) {
        executable.resize(std::strlen(executable.c_str()));
        candidates.push_back(std::filesystem::path{executable}.parent_path() / "../libexec/liquid" /
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

auto excluded_path(std::string_view path,
                   const std::set<std::string>& outputs,
                   const std::filesystem::path& cache,
                   const std::filesystem::path& root) -> bool {
    if (path == ".git" || path.starts_with(".git/") || path == ".liquid-store" ||
        path.starts_with(".liquid-store/") || path == "LIQUID_REPO" || path == "LIQUID_JOURNAL" ||
        path.starts_with(".liquid-build/")) {
        return true;
    }
    if (outputs.contains(std::string{path}))
        return true;
    const auto relative_cache = cache.lexically_relative(root).generic_string();
    return !relative_cache.empty() && !relative_cache.starts_with("../") &&
           (path == relative_cache || path.starts_with(relative_cache + "/"));
}

auto source_files(const std::filesystem::path& root,
                  const std::set<std::string>& outputs,
                  const std::filesystem::path& cache)
    -> liquid::result<std::map<std::string, std::filesystem::path>> {
    auto result = std::map<std::string, std::filesystem::path>{};
    auto code = std::error_code{};
    for (std::filesystem::recursive_directory_iterator iterator{
             root, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        const auto relative = iterator->path().lexically_relative(root).generic_string();
        if (iterator->is_directory(code) && excluded_path(relative, outputs, cache, root)) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_regular_file(code) && !excluded_path(relative, outputs, cache, root))
            result.emplace(relative, iterator->path());
    }
    if (code)
        return std::unexpected(wrap_error(errc::io, code.message(), root));
    return result;
}

runtime::task<void> sync_sources(repository& source,
                                 const std::filesystem::path& root,
                                 const std::filesystem::path& cache,
                                 wrap_state& state) {
    auto files = source_files(root, state.outputs, cache);
    if (!files)
        co_return std::unexpected(files.error());
    for (const auto& old : state.sources) {
        if (files->contains(old))
            continue;
        auto removed = co_await source.remove(old);
        if (!removed && removed.error().code != errc::not_found)
            co_return std::unexpected(removed.error());
    }
    state.sources.clear();
    for (const auto& [relative, path] : *files) {
        auto written = co_await source.write_file(relative, path);
        if (!written)
            co_return std::unexpected(written.error());
        auto status = std::filesystem::status(path);
        auto mode = static_cast<std::uint32_t>(status.permissions()) & 0777U;
        auto mode_set = co_await repository_detail::vfs_access::set_mode(
            source, relative, static_cast<std::uint32_t>(S_IFREG) | mode);
        if (!mode_set)
            co_return std::unexpected(mode_set.error());
        state.sources.insert(relative);
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

runtime::task<void> materialise_result(repository& source, const action_result& value) {
    for (const auto& output : value.outputs) {
        auto contents = co_await blob_contents(source, output.blob);
        if (!contents)
            co_return std::unexpected(contents.error());
        auto written = co_await source.write(output.path, std::move(*contents));
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
        auto contents = co_await blob_contents(source, output.blob);
        if (!contents)
            co_return std::unexpected(contents.error());
        const auto target = working_tree / output.path;
        auto copied = co_await runtime::offload([target,
                                                 bytes = std::move(*contents),
                                                 mode = output.mode]() -> liquid::result<void> {
            auto code = std::error_code{};
            std::filesystem::create_directories(target.parent_path(), code);
            if (code)
                return std::unexpected(wrap_error(errc::io, code.message(), target));
            auto stream = std::ofstream{target, std::ios::binary | std::ios::trunc};
            if (!stream)
                return std::unexpected(wrap_error(errc::io, "cannot materialise output", target));
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            stream.close();
            if (!stream)
                return std::unexpected(wrap_error(errc::io, "cannot materialise output", target));
            std::filesystem::permissions(target,
                                         static_cast<std::filesystem::perms>(mode & 0777U),
                                         std::filesystem::perm_options::replace,
                                         code);
            if (code)
                return std::unexpected(wrap_error(errc::io, code.message(), target));
            return {};
        });
        if (!copied)
            co_return std::unexpected(copied.error());
        if (!*copied)
            co_return std::unexpected(copied->error());
    }
    co_return liquid::result<void>{};
}

runtime::task<void> copy_outputs(repository& source,
                                 const std::filesystem::path& root,
                                 const std::set<std::string>& outputs) {
    for (const auto& path : outputs) {
        auto contents = co_await source.read(path);
        if (!contents) {
            if (contents.error().code == errc::not_found)
                continue;
            co_return std::unexpected(contents.error());
        }
        auto entry = co_await repository_detail::vfs_access::entry(source, path);
        if (!entry)
            co_return std::unexpected(entry.error());
        auto target = root / path;
        auto copied = co_await runtime::offload([target,
                                                 bytes = std::move(*contents),
                                                 mode = entry->mode]() -> liquid::result<void> {
            auto code = std::error_code{};
            std::filesystem::create_directories(target.parent_path(), code);
            if (code)
                return std::unexpected(wrap_error(errc::io, code.message(), target));
            auto output = std::ofstream{target, std::ios::binary | std::ios::trunc};
            if (!output)
                return std::unexpected(wrap_error(errc::io, "cannot materialise output", target));
            output.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
            output.close();
            if (!output)
                return std::unexpected(wrap_error(errc::io, "cannot materialise output", target));
            std::filesystem::permissions(target,
                                         static_cast<std::filesystem::perms>(mode & 0777U),
                                         std::filesystem::perm_options::replace,
                                         code);
            if (code)
                return std::unexpected(wrap_error(errc::io, code.message(), target));
            return {};
        });
        if (!copied)
            co_return std::unexpected(copied.error());
        if (!*copied)
            co_return std::unexpected(copied->error());
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
        if (fields.size() != 6)
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
                                           bool publish) {
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
    auto action =
        co_await repository_detail::build_access::put_record(source, std::move(*action_bytes));
    if (!action)
        co_return std::unexpected(action.error());
    auto value = action_result{
        .action = *action,
        .exit = exit,
        .outputs = std::move(*output_paths),
        .stdout_output = empty_blob,
        .stderr_output = empty_blob,
        .manifest = std::move(*manifest),
        .worker = input.platform.os + "/" + input.platform.arch,
        .observed_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
    auto signed_value = co_await repository_detail::build_access::sign_result(source, value);
    if (!signed_value)
        co_return std::unexpected(signed_value.error());
    auto result_bytes = objects::encode(value);
    if (!result_bytes)
        co_return std::unexpected(result_bytes.error());
    auto stored_result =
        co_await repository_detail::build_access::put_record(source, std::move(*result_bytes));
    if (!stored_result)
        co_return std::unexpected(stored_result.error());
    if (publish) {
        auto candidate = cache.candidate_key(input, sandbox);
        if (!candidate)
            co_return std::unexpected(candidate.error());
        auto published = co_await cache.publish(*candidate, result_id{*stored_result}, value);
        if (!published)
            co_return std::unexpected(published.error());
    }
    co_return saved_action{process.key, *stored_result};
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
    auto value = objects::sandbox_record{
        .mechanism = "wrap user+mount+net namespaces with exec-time hook v1",
        .work_root = "/liquid/work/",
        .toolchain_root = "/usr/",
        .home_root = "/tmp/home/",
        .temporary_root = "/tmp/",
        .fixed_environment = {"PATH", "HOME", "TMPDIR", "LANG", "TZ", "SOURCE_DATE_EPOCH"},
        .network = false};
    auto encoded = objects::encode(value);
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
    if (path.filename().empty() || path.parent_path().filename() != "runs")
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

runtime::task<cache_plan> prepare_cache_plan(repository& source,
                                             build_detail::disk_cache& cache,
                                             const wrap_state& state,
                                             const std::vector<std::string>& command,
                                             const std::map<std::string, std::string>& environment,
                                             digest input_tree,
                                             digest toolchain,
                                             digest sandbox,
                                             const wrap_options& options,
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
        if (!reads_invalidated_output) {
            auto lookup = co_await cache.lookup(input, sandbox);
            if (!lookup)
                co_return std::unexpected(lookup.error());
            if (lookup->hit) {
                output.processes.insert_or_assign(saved.key, 0);
                output.next_actions.push_back({saved.key, lookup->hit->value});
                ++report.hits;
                auto value = co_await load_result(source, lookup->hit->value);
                if (value) {
                    auto materialised = co_await materialise_result(source, *value);
                    if (!materialised)
                        co_return std::unexpected(materialised.error());
                }
                continue;
            }
            if (lookup->why && options.explain)
                report.why.push_back(*lookup->why);
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
                                       wrap_state& state,
                                       const std::filesystem::path& state_path,
                                       const std::filesystem::path& working_tree,
                                       const std::vector<std::string>& command,
                                       const action_input& input,
                                       digest sandbox,
                                       const wrap_options& options,
                                       wrap_report& report) {
    if (!options.cache)
        co_return false;
    auto lookup = co_await cache.lookup(input, sandbox);
    if (!lookup)
        co_return std::unexpected(lookup.error());
    if (!lookup->hit) {
        ++report.misses;
        if (lookup->why && options.explain)
            report.why.push_back(*lookup->why);
        co_return false;
    }
    auto value = co_await load_result(source, lookup->hit->value);
    if (!value)
        co_return std::unexpected(value.error());
    auto materialised = co_await materialise_result(source, *value, working_tree);
    if (!materialised)
        co_return std::unexpected(materialised.error());
    for (const auto& action_output : value->outputs)
        state.outputs.insert(action_output.path);
    report.exit = value->exit;
    report.outer = cache_status::hit;
    ++report.hits;
    report.actions = (state.action_command == command ? state.actions.size() : std::size_t{}) + 1;
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
        for (const auto& [key, status] : plan)
            plan_output << std::hex << key << '\t' << std::dec << status << '\n';
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
    co_return observed_run{run, std::move(observations), std::move(*processes), launch};
}

runtime::task<void> publish_observed_run(repository& source,
                                         build_detail::disk_cache& cache,
                                         wrap_state& state,
                                         const std::filesystem::path& state_path,
                                         const std::filesystem::path& toolchain_path,
                                         const std::filesystem::path& working_tree,
                                         const std::vector<std::string>& command,
                                         const std::map<std::string, std::string>& environment,
                                         digest input_tree,
                                         digest sandbox,
                                         std::optional<digest>& toolchain,
                                         cache_plan& plan,
                                         observed_run& run,
                                         const wrap_options& options,
                                         bool outer_miss_recorded,
                                         wrap_report& report) {
    report.exit = run.launch.exit;
    if (!toolchain) {
        for (const auto& process : run.processes)
            state.tools.insert(process.executable);
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
        if (process.pid == driver_pid || plan.processes.contains(process.key) ||
            !state.tools.contains(process.executable) ||
            has_writing_descendant(process, run.processes, writes)) {
            continue;
        }
        const auto found = writes.find(process.pid);
        if (found == writes.end() || found->second.empty())
            continue;
        auto published = co_await publish_action(source,
                                                 cache,
                                                 process,
                                                 environment,
                                                 input_tree,
                                                 *toolchain,
                                                 sandbox,
                                                 *run.observations,
                                                 found->second,
                                                 *empty_blob,
                                                 0,
                                                 options.cache);
        if (!published) {
            static_cast<void>(remove_run(run.directory));
            co_return std::unexpected(published.error());
        }
        plan.next_actions.push_back(*published);
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
                                               input_tree,
                                               *toolchain,
                                               sandbox,
                                               *run.observations,
                                               all_writes,
                                               *empty_blob,
                                               report.exit,
                                               options.cache);
    if (!outer_saved) {
        static_cast<void>(remove_run(run.directory));
        co_return std::unexpected(outer_saved.error());
    }
    if (!outer_miss_recorded)
        ++report.misses;
    state.actions = std::move(plan.next_actions);
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
    report.actions = state.actions.size() + 1;
    co_return liquid::result<void>{};
}

} // namespace

runtime::task<wrap_report>
wrap(std::filesystem::path working_tree, std::vector<std::string> command, wrap_options options) {
#if defined(_WIN32)
    co_return std::unexpected(
        wrap_error(errc::unsupported, "liquid wrap is unavailable on Windows", "liquid Phase 6"));
#else
    if (command.empty() || command.front().empty())
        co_return std::unexpected(wrap_error(errc::invalid_argument, "wrap command is empty"));
    auto code = std::error_code{};
    working_tree = std::filesystem::canonical(working_tree, code);
    if (code || !std::filesystem::is_directory(working_tree))
        co_return std::unexpected(wrap_error(
            errc::invalid_argument, "wrap working tree is not a directory", working_tree));
    auto cache_directory = options.cache_directory
                               ? liquid::result<std::filesystem::path>{*options.cache_directory}
                               : default_cache_directory(working_tree);
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
            ? co_await repository::open(repository_path)
            : co_await repository::create(
                  repository_path, repository_options{.cache = cache_config{.publish = true}});
    if (!source)
        co_return std::unexpected(source.error());
    auto cache = build_detail::disk_cache{*source};
    auto opened = co_await cache.open();
    if (!opened)
        co_return std::unexpected(opened.error());
    auto synced = co_await sync_sources(*source, working_tree, *cache_directory, *state);
    if (!synced)
        co_return std::unexpected(synced.error());
    for (const auto& saved : state->actions) {
        auto value = co_await load_result(*source, saved.result);
        if (!value)
            continue;
        auto materialised = co_await materialise_result(*source, *value);
        if (!materialised)
            co_return std::unexpected(materialised.error());
    }
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
    auto sandbox = co_await store_sandbox(*source);
    if (!sandbox)
        co_return std::unexpected(sandbox.error());
    auto report = wrap_report{};
    auto outer_miss_recorded = false;
    const auto action_environment = fixed_environment(options);
    auto plan = cache_plan{};
    if (*toolchain) {
        auto prepared = co_await prepare_cache_plan(*source,
                                                    cache,
                                                    *state,
                                                    command,
                                                    action_environment,
                                                    input_tree,
                                                    **toolchain,
                                                    *sandbox,
                                                    options,
                                                    report);
        if (!prepared)
            co_return std::unexpected(prepared.error());
        plan = std::move(*prepared);
    }

    auto outer_input = action_input{.command = command,
                                    .cwd = "",
                                    .env = action_environment,
                                    .outputs = {},
                                    .inputs = input_tree,
                                    .toolchain = *toolchain ? **toolchain : digest{},
                                    .platform = host_platform(),
                                    .timeout = {}};
    if (options.cache && *toolchain) {
        auto served = co_await serve_outer_action(*source,
                                                  cache,
                                                  *state,
                                                  state_path,
                                                  working_tree,
                                                  command,
                                                  outer_input,
                                                  *sandbox,
                                                  options,
                                                  report);
        if (!served)
            co_return std::unexpected(served.error());
        if (*served)
            co_return report;
        outer_miss_recorded = true;
    }

    auto run = co_await execute_observed_run(*source,
                                             *cache_directory,
                                             working_tree,
                                             command,
                                             state->outputs,
                                             plan.processes,
                                             action_environment,
                                             input_tree,
                                             options);
    if (!run)
        co_return std::unexpected(run.error());
    auto published = co_await publish_observed_run(*source,
                                                   cache,
                                                   *state,
                                                   state_path,
                                                   toolchain_path,
                                                   working_tree,
                                                   command,
                                                   action_environment,
                                                   input_tree,
                                                   *sandbox,
                                                   *toolchain,
                                                   plan,
                                                   *run,
                                                   options,
                                                   outer_miss_recorded,
                                                   report);
    if (!published)
        co_return std::unexpected(published.error());
    co_return report;
#endif
}

} // namespace liquid::build
