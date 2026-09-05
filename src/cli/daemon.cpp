#include <array>
#include <charconv>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <format>
#include <limits>
#include <print>
#include <string>
#include <string_view>

#ifndef _WIN32
#include <fcntl.h>
#include <spawn.h>
#include <sys/types.h>
#include <unistd.h>

extern char** environ;
#endif
#ifdef __APPLE__
#include <mach-o/dyld.h>
#endif

#include "liquid/daemon.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto executable = std::filesystem::path{};

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto parse_unsigned(std::string_view text, std::string_view name) -> liquid::result<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto [end, conversion_error] =
        std::from_chars(text.data(), text.data() + text.size(), output);
    if (conversion_error != std::errc{} || end != text.data() + text.size()) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument, "invalid unsigned value", std::string{name}});
    }
    return output;
}

auto environment_unsigned(const char* name, std::uint64_t fallback) -> std::uint64_t {
    const auto* value = std::getenv(name);
    if (!value) {
        return fallback;
    }
    auto parsed = parse_unsigned(value, name);
    return parsed.value_or(fallback);
}

#ifndef _WIN32
auto spawn_daemon(const std::filesystem::path& checkout) -> liquid::result<void> {
    if (executable.empty()) {
        return std::unexpected(liquid::error{
            liquid::errc::internal, "the liquid executable path is unavailable", "daemon start"});
    }
    const auto executable_text = executable.string();
    const auto checkout_text = checkout.string();
    const auto idle_text = std::to_string(environment_unsigned("LIQUID_DAEMON_IDLE_MS", 300000));
    const auto memory_text =
        std::to_string(environment_unsigned("LIQUID_DAEMON_MEMORY", 256ULL * 1024ULL * 1024ULL));
    const auto validation_rate_text =
        std::to_string(environment_unsigned("LIQUID_GRAPH_VALIDATION_RATE", 20));
    const auto validation_interval_text =
        std::to_string(environment_unsigned("LIQUID_GRAPH_VALIDATION_MS", 1000));
    auto actions = posix_spawn_file_actions_t{};
    auto spawn_error = posix_spawn_file_actions_init(&actions);
    const auto actions_ready = spawn_error == 0;
    for (auto descriptor = STDIN_FILENO; spawn_error == 0 && descriptor <= STDERR_FILENO;
         ++descriptor) {
        spawn_error =
            posix_spawn_file_actions_addopen(&actions, descriptor, "/dev/null", O_RDWR, 0);
    }
    auto arguments = std::array{const_cast<char*>(executable_text.c_str()),
                                const_cast<char*>("daemon"),
                                const_cast<char*>("--serve"),
                                const_cast<char*>(checkout_text.c_str()),
                                const_cast<char*>("--idle-ms"),
                                const_cast<char*>(idle_text.c_str()),
                                const_cast<char*>("--memory"),
                                const_cast<char*>(memory_text.c_str()),
                                const_cast<char*>("--validation-rate"),
                                const_cast<char*>(validation_rate_text.c_str()),
                                const_cast<char*>("--validation-ms"),
                                const_cast<char*>(validation_interval_text.c_str()),
                                static_cast<char*>(nullptr)};
    auto child = pid_t{};
    if (spawn_error == 0) {
        spawn_error = ::posix_spawn(
            &child, executable_text.c_str(), &actions, nullptr, arguments.data(), environ);
    }
    if (actions_ready) {
        posix_spawn_file_actions_destroy(&actions);
    }
    if (spawn_error != 0) {
        return std::unexpected(
            liquid::error{liquid::errc::io, std::strerror(spawn_error), "daemon start"});
    }
    for (auto attempt = 0; attempt != 500; ++attempt) {
        auto attached = liquid::runtime::block_on(liquid::daemon::attach(checkout));
        if (attached) {
            return {};
        }
        if (attached.error().code != liquid::errc::unavailable) {
            return std::unexpected(attached.error());
        }
        ::usleep(10000);
    }
    return std::unexpected(liquid::error{
        liquid::errc::unavailable, "daemon did not publish its endpoint", checkout.string()});
}
#endif

} // namespace

auto set_executable(std::filesystem::path path) -> void {
    if (path.has_parent_path()) {
        executable = std::filesystem::absolute(std::move(path));
        return;
    }
#ifdef __APPLE__
    auto size = std::uint32_t{};
    _NSGetExecutablePath(nullptr, &size);
    auto buffer = std::string(size, '\0');
    if (_NSGetExecutablePath(buffer.data(), &size) == 0) {
        executable = std::filesystem::weakly_canonical(buffer.c_str());
        return;
    }
#elif defined(__linux__)
    auto buffer = std::string(4096, '\0');
    const auto count = ::readlink("/proc/self/exe", buffer.data(), buffer.size());
    if (count > 0) {
        buffer.resize(static_cast<std::size_t>(count));
        executable = std::move(buffer);
        return;
    }
#endif
    executable = std::filesystem::absolute(std::move(path));
}

auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void> {
    auto attached = liquid::runtime::block_on(liquid::daemon::attach(checkout));
    if (attached) {
        return {};
    }
    if (attached.error().code != liquid::errc::unavailable) {
        return std::unexpected(attached.error());
    }
#ifdef _WIN32
    return std::unexpected(attached.error());
#else
    return spawn_daemon(checkout);
#endif
}

auto daemon_command(int argc, char** argv) -> int {
    auto checkout = std::filesystem::current_path();
    if (argc >= 1 && std::string_view{argv[0]} == "--serve") {
        if (argc < 2) {
            return usage("daemon --serve <checkout> [--idle-ms <n>] [--memory <bytes>] "
                         "[--validation-rate <n>] [--validation-ms <n>]");
        }
        checkout = argv[1];
        auto options = liquid::daemon_options{};
        for (auto index = 2; index < argc; ++index) {
            const auto argument = std::string_view{argv[index]};
            if ((argument == "--idle-ms" || argument == "--memory" ||
                 argument == "--validation-rate" || argument == "--validation-ms") &&
                index + 1 < argc) {
                auto parsed = parse_unsigned(argv[++index], argument);
                if (!parsed) {
                    return fail(parsed.error());
                }
                if (argument == "--idle-ms") {
                    options.idle_timeout = std::chrono::milliseconds{*parsed};
                } else if (argument == "--memory") {
                    options.graph_memory = *parsed;
                } else if (argument == "--validation-rate") {
                    if (*parsed > std::numeric_limits<std::uint32_t>::max()) {
                        return usage("daemon validation rate is too large");
                    }
                    options.graph_validation_rate = static_cast<std::uint32_t>(*parsed);
                } else {
                    options.graph_validation_interval = std::chrono::milliseconds{*parsed};
                }
                continue;
            }
            return usage("daemon --serve <checkout> [--idle-ms <n>] [--memory <bytes>] "
                         "[--validation-rate <n>] [--validation-ms <n>]");
        }
        auto started = liquid::runtime::block_on(liquid::daemon::start(checkout, options));
        if (!started) {
            return fail(started.error());
        }
        auto waited = liquid::runtime::block_on(started->wait());
        return waited ? 0 : fail(waited.error());
    }
    if (argc == 1 && std::string_view{argv[0]} == "--shutdown") {
        auto attached = liquid::runtime::block_on(liquid::daemon::attach(checkout));
        if (!attached) {
            return fail(attached.error());
        }
        auto stopped = liquid::runtime::block_on(attached->shutdown());
        return stopped ? 0 : fail(stopped.error());
    }
    if (argc != 0) {
        return usage("daemon [--shutdown]");
    }
    auto ready = ensure_daemon(checkout);
    if (!ready) {
        return fail(ready.error());
    }
    auto attached = liquid::runtime::block_on(liquid::daemon::attach(checkout));
    if (!attached) {
        return fail(attached.error());
    }
    auto status = liquid::runtime::block_on(attached->status());
    if (!status) {
        return fail(status.error());
    }
    std::println("running=true");
    std::println("pid={}", status->process_id);
    std::println("endpoint={}", status->endpoint);
    return 0;
}

} // namespace liquid_cli
