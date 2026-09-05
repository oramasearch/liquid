#include "build/tests.hpp"

#include <algorithm>
#include <cctype>
#include <charconv>
#include <fstream>
#include <map>
#include <ranges>
#include <sstream>
#include <system_error>

#include "build/events.hpp"
#include "build/repository_access.hpp"
#include "build/wrap.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"
#include "vfs/repository_access.hpp"

namespace liquid::build_detail {
namespace {

auto test_error(errc code, std::string message, const std::filesystem::path& context = {})
    -> error {
    return {code, std::move(message), context.string()};
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

auto unhex(std::string_view input) -> result<std::string> {
    if (input.size() % 2 != 0) {
        return std::unexpected(test_error(errc::corrupt, "invalid test target string"));
    }
    auto output = std::string{};
    output.reserve(input.size() / 2);
    for (auto offset = std::size_t{}; offset != input.size(); offset += 2) {
        auto value = unsigned{};
        const auto parsed =
            std::from_chars(input.data() + offset, input.data() + offset + 2, value, 16);
        if (parsed.ec != std::errc{} || parsed.ptr != input.data() + offset + 2) {
            return std::unexpected(test_error(errc::corrupt, "invalid test target string"));
        }
        output.push_back(static_cast<char>(value));
    }
    return output;
}

auto target_file(const std::filesystem::path& root, std::string_view name)
    -> result<std::filesystem::path> {
    auto id = hash(algorithm::blake3,
                   std::span{reinterpret_cast<const std::byte*>(name.data()), name.size()});
    if (!id) {
        return std::unexpected(id.error());
    }
    auto text = to_string(*id);
    std::ranges::replace(text, ':', '-');
    return root / ".liquid-build" / "tests" / (text + ".test");
}

auto write_target(const std::filesystem::path& path, const test_target& target) -> result<void> {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    if (code) {
        return std::unexpected(test_error(errc::io, code.message(), path.parent_path()));
    }
    auto temporary = path;
    temporary += ".tmp";
    auto output = std::ofstream{temporary, std::ios::trunc};
    if (!output) {
        return std::unexpected(test_error(errc::io, "cannot write test target", temporary));
    }
    output << "V\t1\n";
    output << "N\t" << hex(target.name) << '\n';
    output << "W\t" << hex(target.cwd) << '\n';
    output << "T\t" << to_string(target.toolchain) << '\n';
    output << "O\t" << hex(target.platform.os) << '\n';
    output << "A\t" << hex(target.platform.arch) << '\n';
    output << "X\t" << (target.timeout ? target.timeout->count() : 0) << '\n';
    for (const auto& argument : target.command) {
        output << "C\t" << hex(argument) << '\n';
    }
    for (const auto& label : target.labels) {
        output << "L\t" << hex(label) << '\n';
    }
    for (const auto& input : target.inputs) {
        output << "I\t" << hex(input) << '\n';
    }
    output.close();
    if (!output) {
        return std::unexpected(test_error(errc::io, "cannot flush test target", temporary));
    }
    std::filesystem::rename(temporary, path, code);
    if (code) {
        std::filesystem::remove(path, code);
        code.clear();
        std::filesystem::rename(temporary, path, code);
    }
    if (code) {
        return std::unexpected(test_error(errc::io, code.message(), path));
    }
    return {};
}

auto read_target(const std::filesystem::path& path) -> result<test_target> {
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(test_error(errc::io, "cannot read test target", path));
    }
    auto output = test_target{};
    auto version = std::uint64_t{};
    for (auto line = std::string{}; std::getline(input, line);) {
        if (line.size() < 2 || line[1] != '\t') {
            return std::unexpected(test_error(errc::corrupt, "invalid test target", path));
        }
        const auto value = std::string_view{line}.substr(2);
        if (line[0] == 'V') {
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), version);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size()) {
                return std::unexpected(
                    test_error(errc::corrupt, "invalid test target version", path));
            }
            continue;
        }
        if (line[0] == 'T') {
            auto parsed = parse_digest(value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            output.toolchain = *parsed;
            continue;
        }
        if (line[0] == 'X') {
            auto seconds = std::int64_t{};
            const auto parsed = std::from_chars(value.data(), value.data() + value.size(), seconds);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                seconds < 0) {
                return std::unexpected(test_error(errc::corrupt, "invalid test timeout", path));
            }
            if (seconds != 0) {
                output.timeout = std::chrono::seconds{seconds};
            }
            continue;
        }
        auto decoded = unhex(value);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        switch (line[0]) {
        case 'N':
            output.name = std::move(*decoded);
            break;
        case 'W':
            output.cwd = std::move(*decoded);
            break;
        case 'O':
            output.platform.os = std::move(*decoded);
            break;
        case 'A':
            output.platform.arch = std::move(*decoded);
            break;
        case 'C':
            output.command.push_back(std::move(*decoded));
            break;
        case 'L':
            output.labels.push_back(std::move(*decoded));
            break;
        case 'I':
            output.inputs.push_back(std::move(*decoded));
            break;
        default:
            return std::unexpected(test_error(errc::corrupt, "unknown test target field", path));
        }
    }
    if (!input.eof() || version != 1 || output.name.empty() || output.command.empty()) {
        return std::unexpected(test_error(errc::corrupt, "incomplete test target", path));
    }
    return output;
}

auto read_targets(const std::filesystem::path& root) -> result<std::vector<test_target>> {
    const auto directory = root / ".liquid-build" / "tests";
    auto output = std::vector<test_target>{};
    auto code = std::error_code{};
    if (!std::filesystem::exists(directory, code)) {
        return output;
    }
    for (const auto& entry : std::filesystem::directory_iterator{directory, code}) {
        if (!entry.is_regular_file() || entry.path().extension() != ".test") {
            continue;
        }
        auto target = read_target(entry.path());
        if (!target) {
            return std::unexpected(target.error());
        }
        output.push_back(std::move(*target));
    }
    if (code) {
        return std::unexpected(test_error(errc::io, code.message(), directory));
    }
    std::ranges::sort(output, {}, &test_target::name);
    return output;
}

auto cmake_arguments(std::string_view line) -> result<std::vector<std::string>> {
    const auto open = line.find('(');
    const auto close = line.rfind(')');
    if (open == std::string_view::npos || close == std::string_view::npos || close <= open) {
        return std::unexpected(test_error(errc::corrupt, "invalid CMake test registration"));
    }
    auto output = std::vector<std::string>{};
    for (auto index = open + 1; index < close;) {
        while (index < close && std::isspace(static_cast<unsigned char>(line[index]))) {
            ++index;
        }
        if (index == close) {
            break;
        }
        auto value = std::string{};
        if (line[index] == '"') {
            ++index;
            while (index < close && line[index] != '"') {
                if (line[index] == '\\' && index + 1 < close) {
                    ++index;
                }
                value.push_back(line[index++]);
            }
            if (index == close) {
                return std::unexpected(test_error(errc::corrupt, "unterminated CMake test string"));
            }
            ++index;
        } else if (line[index] == '[') {
            const auto marker_end = line.find('[', index + 1);
            if (marker_end == std::string_view::npos) {
                return std::unexpected(test_error(errc::corrupt, "invalid CMake bracket string"));
            }
            const auto marker = line.substr(index + 1, marker_end - index - 1);
            const auto terminator = "]" + std::string{marker} + "]";
            const auto value_end = line.find(terminator, marker_end + 1);
            if (value_end == std::string_view::npos) {
                return std::unexpected(
                    test_error(errc::corrupt, "unterminated CMake bracket string"));
            }
            value = line.substr(marker_end + 1, value_end - marker_end - 1);
            index = value_end + terminator.size();
        } else {
            const auto value_end = line.find_first_of(" \t)", index);
            const auto end = value_end == std::string_view::npos ? close : value_end;
            value = line.substr(index, end - index);
            index = end;
        }
        output.push_back(std::move(value));
    }
    return output;
}

struct discovered_test {
    std::vector<std::string> command;
    std::vector<std::string> labels;
};

auto scan_cmake_tests(const std::filesystem::path& root)
    -> result<std::map<std::string, discovered_test>> {
    auto output = std::map<std::string, discovered_test>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 root, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        const auto relative = iterator->path().lexically_relative(root).generic_string();
        if (iterator->is_directory(code) &&
            (relative == ".git" || relative == ".liquid" || relative.starts_with(".git/") ||
             relative.starts_with(".liquid/"))) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (!iterator->is_regular_file(code) ||
            iterator->path().filename() != "CTestTestfile.cmake") {
            continue;
        }
        auto input = std::ifstream{iterator->path()};
        for (auto line = std::string{}; std::getline(input, line);) {
            if (line.starts_with("add_test(")) {
                auto arguments = cmake_arguments(line);
                if (!arguments || arguments->size() < 2) {
                    return std::unexpected(arguments ? test_error(errc::corrupt, "empty CMake test")
                                                     : arguments.error());
                }
                auto name = arguments->front();
                arguments->erase(arguments->begin());
                output[name].command = std::move(*arguments);
                continue;
            }
            if (!line.starts_with("set_tests_properties(")) {
                continue;
            }
            auto arguments = cmake_arguments(line);
            if (!arguments || arguments->size() < 4) {
                continue;
            }
            const auto properties = std::ranges::find(*arguments, "PROPERTIES");
            if (properties == arguments->end()) {
                continue;
            }
            const auto labels = std::ranges::find(properties + 1, arguments->end(), "LABELS");
            if (labels == arguments->end() || labels + 1 == arguments->end()) {
                continue;
            }
            auto values = std::stringstream{*(labels + 1)};
            for (auto label = std::string{}; std::getline(values, label, ';');) {
                output[arguments->front()].labels.push_back(std::move(label));
            }
        }
    }
    if (code) {
        return std::unexpected(test_error(errc::io, code.message(), root));
    }
    return output;
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

auto matches(const test_target& target, std::string_view filter) -> bool {
    if (filter.empty() || target.name.contains(filter)) {
        return true;
    }
    return std::ranges::any_of(
        target.labels, [filter](const std::string& label) { return label.contains(filter); });
}

auto test_output(repository& source, const action_result& value) -> runtime::task<std::string> {
    auto stdout_bytes = co_await source.blob(value.stdout_output);
    auto stderr_bytes = co_await source.blob(value.stderr_output);
    if (!stdout_bytes || !stderr_bytes) {
        co_return std::unexpected(!stdout_bytes ? stdout_bytes.error() : stderr_bytes.error());
    }
    auto output =
        std::string{reinterpret_cast<const char*>(stdout_bytes->data()), stdout_bytes->size()};
    output.append(reinterpret_cast<const char*>(stderr_bytes->data()), stderr_bytes->size());
    co_return output;
}

runtime::task<void> run_tests(event_producer producer, repository& source, std::string filter) {
    auto observed = co_await producer.await_observer();
    if (!observed) {
        co_return std::unexpected(observed.error());
    }
    const auto started = std::chrono::steady_clock::now();
    auto targets = co_await load_tests(source);
    if (!targets) {
        producer.fail(targets.error(), {});
        co_return {};
    }
    auto head = co_await source.head();
    if (!head) {
        producer.fail(head.error(), {});
        co_return {};
    }
    auto encoded_change = co_await repository_detail::build_access::get(source, head->value);
    if (!encoded_change) {
        producer.fail(encoded_change.error(), {});
        co_return {};
    }
    auto change = objects::decode_change(*encoded_change);
    if (!change) {
        producer.fail(change.error(), {});
        co_return {};
    }
    auto summary = build_summary{};
    for (auto& target : *targets) {
        if (!matches(target, filter)) {
            continue;
        }
        ++summary.actions;
        producer.publish({.kind = build_event_kind::action_queued});
        producer.publish({.kind = build_event_kind::action_started,
                          .execution = action_execution{action_execution_kind::local, {}}});
        auto id = co_await build::execute(source,
                                          {.command = target.command,
                                           .cwd = target.cwd,
                                           .outputs = {},
                                           .inputs = change->root,
                                           .toolchain = target.toolchain,
                                           .platform = target.platform,
                                           .timeout = target.timeout});
        if (!id) {
            ++summary.failures;
            producer.publish({.kind = build_event_kind::action_failed,
                              .diagnostics = {{.severity = frontend_diagnostic_severity::error,
                                               .message = id.error().message}}});
            producer.publish(
                {.kind = build_event_kind::test_result,
                 .test =
                     test_result{target.name, test_status::failed, id.error().message, {}, false}});
            continue;
        }
        auto value = co_await build::result(source, *id);
        if (!value) {
            producer.fail(value.error(), summary);
            co_return {};
        }
        auto output = co_await test_output(source, *value);
        if (!output) {
            producer.fail(output.error(), summary);
            co_return {};
        }
        const auto duration = value->timing.queued + value->timing.sandbox + value->timing.exec +
                              value->timing.hash + value->timing.store;
        const auto hit = value->cache == cache_status::hit;
        if (hit) {
            ++summary.hits;
            producer.publish(
                {.kind = build_event_kind::action_hit, .action = value->action, .result = *id});
        } else if (value->exit == 0) {
            producer.publish({.kind = build_event_kind::action_done,
                              .action = value->action,
                              .result = *id,
                              .outputs = value->outputs});
        } else {
            producer.publish({.kind = build_event_kind::action_failed,
                              .action = value->action,
                              .result = *id,
                              .diagnostics = {{.severity = frontend_diagnostic_severity::error,
                                               .message = *output}}});
        }
        if (value->exit != 0) {
            ++summary.failures;
        }
        target.inputs.clear();
        for (const auto& input : value->manifest) {
            target.inputs.push_back(input.path);
        }
        auto stored = co_await store_test(source, target);
        if (!stored) {
            producer.fail(stored.error(), summary);
            co_return {};
        }
        producer.publish(
            {.kind = build_event_kind::test_result,
             .test = test_result{target.name,
                                 value->exit == 0 ? test_status::passed : test_status::failed,
                                 std::move(*output),
                                 duration,
                                 hit}});
    }
    summary.exit = summary.failures == 0 ? 0 : 1;
    summary.duration = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - started);
    producer.finish(summary);
    co_return {};
}

} // namespace

runtime::task<void> store_test(repository& source, test_target target) {
    if (target.name.empty() || target.command.empty() || target.command.front().empty()) {
        co_return std::unexpected(test_error(errc::invalid_argument, "test target is incomplete"));
    }
    if (target.platform.os.empty() || target.platform.arch.empty()) {
        target.platform = host_platform();
    }
    auto root = repository_detail::build_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto path = target_file(*root, target.name);
    if (!path) {
        co_return std::unexpected(path.error());
    }
    auto written = co_await runtime::offload([path = std::move(*path), target = std::move(target)] {
        return write_target(path, target);
    });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return std::move(*written);
}

runtime::task<std::vector<test_target>> load_tests(repository& source) {
    auto root = repository_detail::build_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto loaded = co_await runtime::offload([root = *root] { return read_targets(root); });
    if (!loaded) {
        co_return std::unexpected(loaded.error());
    }
    co_return std::move(*loaded);
}

runtime::task<void>
discover_cmake_tests(repository& source, std::filesystem::path working_tree, digest toolchain) {
    auto discovered =
        co_await runtime::offload([working_tree] { return scan_cmake_tests(working_tree); });
    if (!discovered) {
        co_return std::unexpected(discovered.error());
    }
    if (!*discovered) {
        co_return std::unexpected(discovered->error());
    }
    if ((*discovered)->empty()) {
        co_return {};
    }
    auto targets = std::vector<test_target>{};
    auto copied_executable = false;
    for (auto& [name, value] : **discovered) {
        if (value.command.empty()) {
            continue;
        }
        auto executable = std::filesystem::path{value.command.front()};
        constexpr auto wrapped_root = std::string_view{"/liquid/work"};
        if (executable.is_absolute() &&
            executable.lexically_normal().string().starts_with(wrapped_root)) {
            auto relative = executable.lexically_relative(wrapped_root);
            if (!relative.empty() && *relative.begin() != "..") {
                executable = working_tree / relative;
            }
        }
        if (executable.is_absolute()) {
            const auto relative = executable.lexically_relative(working_tree);
            if (!relative.empty() && *relative.begin() != ".." &&
                std::filesystem::is_regular_file(executable)) {
                auto copied = co_await source.write_file(relative.generic_string(), executable);
                if (!copied) {
                    co_return std::unexpected(copied.error());
                }
                auto mode_set = co_await repository_detail::vfs_access::set_mode(
                    source, relative.generic_string(), 0100755);
                if (!mode_set) {
                    co_return std::unexpected(mode_set.error());
                }
                copied_executable = true;
                value.command.front() = relative.generic_string();
                if (!value.command.front().starts_with("./")) {
                    value.command.front() = "./" + value.command.front();
                }
            }
        }
        auto target = test_target{.name = std::move(name),
                                  .command = std::move(value.command),
                                  .labels = std::move(value.labels),
                                  .toolchain = toolchain,
                                  .platform = host_platform()};
        target.inputs.push_back(target.command.front());
        targets.push_back(std::move(target));
    }
    if (copied_executable) {
        auto recorded = co_await source.cut("record wrapped test targets");
        if (!recorded) {
            co_return std::unexpected(recorded.error());
        }
    }
    for (auto& target : targets) {
        if (std::filesystem::path{target.command.front()}.is_relative()) {
            auto head = co_await source.log({.first_parent = true});
            if (!head || head->empty()) {
                co_return std::unexpected(
                    head ? test_error(errc::not_found, "repository has no test input tree")
                         : head.error());
            }
            auto prepared = co_await build::phase2_toolchain(
                source, target.toolchain, head->front().root, target.cwd, target.command.front());
            if (!prepared) {
                co_return std::unexpected(prepared.error());
            }
            target.toolchain = prepared->first;
            target.command.front() = std::move(prepared->second);
        }
        auto stored = co_await store_test(source, std::move(target));
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
    }
    co_return {};
}

result<build_handle> start_tests(repository& source, std::string filter) {
    auto stream = event_access::create();
    auto producer = stream.producer;
    runtime::spawn_serial(run_tests(producer, source, std::move(filter)),
                          [producer](liquid::result<void> completed) {
                              if (!completed) {
                                  producer.fail(completed.error(), {});
                              }
                          });
    return std::move(stream.handle);
}

} // namespace liquid::build_detail

namespace liquid {

runtime::task<void> repository::register_test(test_target target) {
    co_return co_await build_detail::store_test(*this, std::move(target));
}

runtime::task<std::vector<test_target>> repository::tests() {
    co_return co_await build_detail::load_tests(*this);
}

runtime::task<repository> repository::open_build(std::filesystem::path working_tree,
                                                 repository_options options) {
    auto cache = build::wrap_cache_directory(working_tree);
    if (!cache) {
        co_return std::unexpected(cache.error());
    }
    co_return co_await repository::open(*cache / "repository", std::move(options));
}

result<build_handle> repository::run_tests(std::string filter) {
    return build_detail::start_tests(*this, std::move(filter));
}

} // namespace liquid
