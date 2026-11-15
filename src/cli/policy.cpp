// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/build.hpp"
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

auto scope_name(liquid::policy_scope scope) -> std::string_view {
    switch (scope) {
    case liquid::policy_scope::paths:
        return "paths";
    case liquid::policy_scope::toolchain:
        return "toolchain";
    case liquid::policy_scope::all_inputs:
        return "all inputs";
    }
    return "unknown";
}

auto action_name(liquid::policy_on_missing action) -> std::string_view {
    return action == liquid::policy_on_missing::refuse ? "refuse" : "warn";
}

auto join(const std::vector<std::string>& values) -> std::string {
    auto output = std::string{};
    for (const auto& value : values) {
        if (!output.empty()) {
            output += ',';
        }
        output += value;
    }
    return output;
}

void print_report(const liquid::policy_report& report) {
    std::println("policy={} allowed={}", liquid::to_string(report.policy), report.allowed);
    std::println("approved-builders={}", join(report.config.approved_builders));
    for (std::size_t index = 0; index < report.config.rules.size(); ++index) {
        const auto& rule = report.config.rules[index];
        std::println("rule[{}] scope={} paths={} on_missing={}",
                     index,
                     scope_name(rule.scope),
                     join(rule.paths),
                     action_name(rule.on_missing));
        for (const auto& requirement : rule.require) {
            std::println("  require profile={} from={} min_count={}",
                         requirement.profile,
                         join(requirement.from),
                         requirement.min_count);
        }
    }
    for (const auto& input : report.inputs) {
        std::println("input={} subject={} rule={} satisfied={}",
                     input.input,
                     liquid::to_string(input.subject),
                     input.rule,
                     input.satisfied);
        for (const auto& requirement : input.requirements) {
            std::println("  profile={} count={} min_count={} identities={} satisfied={}{}{}",
                         requirement.requirement.profile,
                         requirement.count,
                         requirement.requirement.min_count,
                         join(requirement.identities),
                         requirement.satisfied,
                         requirement.reason.empty() ? "" : " reason=",
                         requirement.reason);
        }
    }
    for (const auto& warning : report.warnings) {
        std::println(stderr, "warning: {}", warning);
    }
}

} // namespace

auto policy(int argc, char** argv) -> int {
    if (argc < 1 || argc > 2) {
        return usage("policy requires check <target> or explain [target]");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "check" && argc != 2) {
        return usage("policy check requires a target");
    }
    if (command != "check" && command != "explain") {
        return usage("policy requires check or explain");
    }
    auto report = liquid::result<liquid::policy_report>{std::unexpected(
        liquid::error{liquid::errc::invalid_argument, "policy command was not evaluated", {}})};
    if (argc == 2) {
        report = liquid::runtime::block_on(
            liquid::build::check_policy(std::filesystem::current_path(), std::string{argv[1]}));
    } else {
        report = liquid::runtime::block_on(
            liquid::build::explain_policy(std::filesystem::current_path()));
    }
    if (!report) {
        return fail(report.error());
    }
    print_report(*report);
    return report->allowed ? 0 : 1;
}

} // namespace liquid_cli
