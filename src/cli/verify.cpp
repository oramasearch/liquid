#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/hash.hpp"
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

auto digest_text(const std::optional<liquid::digest>& value) -> std::string {
    return value ? liquid::to_string(*value) : "missing";
}

} // namespace

auto verify(int argc, char** argv) -> int {
    auto options = liquid::verify_options{};
    auto selector = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--explain") {
            options.explain = true;
            continue;
        }
        if (selector) {
            return usage("verify accepts one of <action>|--all|--changed");
        }
        if (argument == "--all") {
            options.selection = liquid::verify_selection::all;
            selector = true;
            continue;
        }
        if (argument == "--changed") {
            options.selection = liquid::verify_selection::changed;
            selector = true;
            continue;
        }
        auto action = liquid::parse_digest(argument);
        if (!action) {
            return usage("verify requires an action digest, --all, or --changed");
        }
        options.selection = liquid::verify_selection::action;
        options.action = *action;
        selector = true;
    }
    if (!selector) {
        return usage("verify requires an action digest, --all, or --changed");
    }
    auto report =
        liquid::runtime::block_on(liquid::build::verify(std::filesystem::current_path(), options));
    if (!report) {
        return fail(report.error());
    }
    auto divergent = false;
    for (const auto& action : report->actions) {
        std::println("action={} result={} attestation={} agreement={}",
                     liquid::to_string(action.action),
                     liquid::to_string(action.result),
                     liquid::to_string(action.attestation),
                     action.agreement);
        if (action.agreement) {
            continue;
        }
        divergent = true;
        for (const auto& difference : action.differences) {
            std::println(stderr,
                         "critical: action={} output={} cached={} reexecuted={}",
                         liquid::to_string(action.action),
                         difference.output,
                         digest_text(difference.cached),
                         digest_text(difference.reexecuted));
            if (options.explain && difference.byte_offset) {
                const auto cached = difference.cached_byte
                                        ? std::format("{:02x}", *difference.cached_byte)
                                        : "missing";
                const auto replayed = difference.reexecuted_byte
                                          ? std::format("{:02x}", *difference.reexecuted_byte)
                                          : "missing";
                std::println(stderr,
                             "diff: output={} offset={} cached={} reexecuted={}",
                             difference.output,
                             *difference.byte_offset,
                             cached,
                             replayed);
            }
        }
    }
    std::println("considered={} verified={} divergent={}",
                 report->considered,
                 report->actions.size(),
                 divergent);
    return divergent ? 1 : 0;
}

} // namespace liquid_cli
