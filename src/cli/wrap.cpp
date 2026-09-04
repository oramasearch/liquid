#include <charconv>
#include <filesystem>
#include <print>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

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

} // namespace

auto wrap(int argc, char** argv) -> int {
    auto options = liquid::wrap_options{};
    auto command = std::vector<std::string>{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (command.empty() && argument == "--no-cache") {
            options.cache = false;
            continue;
        }
        if (command.empty() && argument == "--no-verify") {
            options.verify = false;
            continue;
        }
        if (command.empty() && argument == "--explain") {
            options.explain = true;
            continue;
        }
        if (command.empty() && argument == "--jobs") {
            if (++index == argc)
                return usage("wrap --jobs requires a positive count");
            const auto value = std::string_view{argv[index]};
            const auto parsed =
                std::from_chars(value.data(), value.data() + value.size(), options.jobs);
            if (parsed.ec != std::errc{} || parsed.ptr != value.data() + value.size() ||
                options.jobs == 0) {
                return usage("wrap --jobs requires a positive count");
            }
            continue;
        }
        if (command.empty() && argument == "--")
            continue;
        command.emplace_back(argument);
    }
    if (command.empty())
        return usage("wrap requires a build command");
    auto result = liquid::runtime::block_on(
        liquid::build::wrap(std::filesystem::current_path(), std::move(command), options));
    if (!result)
        return fail(result.error());
    std::println("actions={} hits={} misses={} processes_started={} outer={}",
                 result->actions,
                 result->hits,
                 result->misses,
                 result->processes_started,
                 result->outer == liquid::cache_status::hit ? "hit" : "miss");
    if (options.explain) {
        for (const auto& miss : result->why)
            std::println("miss: {}", miss.path);
    }
    return result->exit;
}

} // namespace liquid_cli
