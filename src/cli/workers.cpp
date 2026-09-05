#include <charconv>
#include <condition_variable>
#include <cstdlib>
#include <filesystem>
#include <limits>
#include <mutex>
#include <optional>
#include <print>
#include <string>
#include <string_view>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"
#include "liquid/workers.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto environment(std::string_view name) -> std::string {
    const auto* value = std::getenv(std::string{name}.c_str());
    return value == nullptr ? std::string{} : std::string{value};
}

auto milliseconds(std::string_view value, std::string_view option)
    -> liquid::result<std::chrono::milliseconds> {
    auto count = std::uint64_t{};
    const auto parsed = std::from_chars(value.data(), value.data() + value.size(), count);
    if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() || count == 0 ||
        count > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max())) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             std::string{option} + " requires a positive count",
                                             std::string{value}});
    }
    return std::chrono::milliseconds{static_cast<std::int64_t>(count)};
}

auto size_class(std::string_view value) -> liquid::result<liquid::action_size_class> {
    if (value == "tiny") {
        return liquid::action_size_class::tiny;
    }
    if (value == "short") {
        return liquid::action_size_class::short_action;
    }
    if (value == "medium") {
        return liquid::action_size_class::medium;
    }
    if (value == "long") {
        return liquid::action_size_class::long_action;
    }
    return std::unexpected(liquid::error{
        liquid::errc::invalid_argument, "worker size class is invalid", std::string{value}});
}

auto run_service(liquid::runtime::task<void> service) -> liquid::result<void> {
    auto mutex = std::mutex{};
    auto condition = std::condition_variable{};
    auto outcome = std::optional<liquid::result<void>>{};
    liquid::runtime::spawn(std::move(service), [&](liquid::result<void> value) {
        {
            auto lock = std::scoped_lock{mutex};
            outcome.emplace(std::move(value));
        }
        condition.notify_one();
    });
    auto lock = std::unique_lock{mutex};
    condition.wait(lock, [&] { return outcome.has_value(); });
    return std::move(*outcome);
}

} // namespace

auto scheduler(int argc, char** argv) -> int {
    auto options = liquid::scheduler_options{.store_directory =
                                                 std::filesystem::current_path() / ".liquid-store"};
    if (const auto value = environment("LIQUID_SCHEDULER_LISTEN"); !value.empty()) {
        options.listen = value;
    }
    if (const auto value = environment("LIQUID_CONTROL_LISTEN"); !value.empty()) {
        options.control_listen = value;
    }
    if (const auto value = environment("LIQUID_INSTANCE"); !value.empty()) {
        options.instance_name = value;
    }
    if (const auto value = environment("LIQUID_SCHEDULER_STORE"); !value.empty()) {
        options.store_directory = value;
    }
    if (const auto value = environment("LIQUID_SCHEDULER_LEASE_MS"); !value.empty()) {
        auto parsed = milliseconds(value, "LIQUID_SCHEDULER_LEASE_MS");
        if (!parsed) {
            return usage(parsed.error().message);
        }
        options.lease_duration = *parsed;
    }
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--listen" && ++index < argc) {
            options.listen = argv[index];
        } else if (argument == "--control-listen" && ++index < argc) {
            options.control_listen = argv[index];
        } else if (argument == "--instance" && ++index < argc) {
            options.instance_name = argv[index];
        } else if (argument == "--store" && ++index < argc) {
            options.store_directory = argv[index];
        } else if (argument == "--lease-ms" && ++index < argc) {
            auto value = milliseconds(argv[index], "--lease-ms");
            if (!value) {
                return usage(value.error().message);
            }
            options.lease_duration = *value;
        } else {
            return usage("scheduler --listen <address> [--control-listen <address>] "
                         "[--store <path>] [--instance <name>] [--lease-ms <count>]");
        }
    }
    if (options.listen.empty()) {
        return usage("scheduler --listen requires an address");
    }
    std::println("scheduler listening on {}", options.listen);
    auto served = run_service(liquid::serve_scheduler(std::move(options)));
    return served ? 0 : fail(served.error());
}

auto worker(int argc, char** argv) -> int {
    auto options =
        liquid::worker_options{.scheduler = {.endpoint = environment("LIQUID_SCHEDULER"),
                                             .instance_name = environment("LIQUID_INSTANCE"),
                                             .backend = liquid::remote_backend::liquid_scheduler},
                               .root_directory = std::filesystem::current_path()};
    if (const auto value = environment("LIQUID_CONTROL_LISTEN"); !value.empty()) {
        options.control_listen = value;
    }
    if (const auto value = environment("LIQUID_WORKER_ROOT"); !value.empty()) {
        options.root_directory = value;
    }
    if (const auto value = environment("LIQUID_WORKER_SIZE_CLASS"); !value.empty()) {
        auto parsed = size_class(value);
        if (!parsed) {
            return usage(parsed.error().message);
        }
        options.size_class = *parsed;
    }
    if (const auto value = environment("LIQUID_WORKER_POLL_MS"); !value.empty()) {
        auto parsed = milliseconds(value, "LIQUID_WORKER_POLL_MS");
        if (!parsed) {
            return usage(parsed.error().message);
        }
        options.poll_interval = *parsed;
    }
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--scheduler" && ++index < argc) {
            options.scheduler.endpoint = argv[index];
        } else if (argument == "--control-listen" && ++index < argc) {
            options.control_listen = argv[index];
        } else if (argument == "--root" && ++index < argc) {
            options.root_directory = argv[index];
        } else if (argument == "--instance" && ++index < argc) {
            options.scheduler.instance_name = argv[index];
        } else if (argument == "--size-class" && ++index < argc) {
            auto parsed = size_class(argv[index]);
            if (!parsed) {
                return usage(parsed.error().message);
            }
            options.size_class = *parsed;
        } else if (argument == "--poll-ms" && ++index < argc) {
            auto parsed = milliseconds(argv[index], "--poll-ms");
            if (!parsed) {
                return usage(parsed.error().message);
            }
            options.poll_interval = *parsed;
        } else {
            return usage("worker --scheduler <address> [--root <path>] "
                         "[--size-class <tiny|short|medium|long>] [--poll-ms <count>]");
        }
    }
    if (options.scheduler.endpoint.empty()) {
        return usage("worker --scheduler requires an address");
    }
    std::println("worker connecting to {}", options.scheduler.endpoint);
    auto served = run_service(liquid::run_worker(std::move(options)));
    return served ? 0 : fail(served.error());
}

} // namespace liquid_cli
