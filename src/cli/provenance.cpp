// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

} // namespace

auto provenance(int argc, char** argv) -> int {
    auto dsse = false;
    auto target = std::string_view{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--dsse") {
            dsse = true;
        } else if (argument.starts_with('-')) {
            return usage("provenance accepts only --dsse and one result or path");
        } else if (!target.empty()) {
            return usage("provenance accepts one result or path");
        } else {
            target = argument;
        }
    }
    if (target.empty()) {
        return usage("provenance requires a result digest or artifact path");
    }
    const auto root = std::filesystem::current_path();
    auto report = liquid::result<liquid::provenance_report>{
        std::unexpected(liquid::error{liquid::errc::not_found, "provenance target not found", {}})};
    auto parsed = liquid::parse_digest(target);
    if (std::filesystem::exists(root / "LIQUID_REPO")) {
        auto repository = liquid::runtime::block_on(liquid::repository::open(root));
        if (!repository) {
            return fail(repository.error());
        }
        report = parsed ? liquid::runtime::block_on(
                              liquid::build::verify_provenance(*repository, {*parsed}))
                        : liquid::runtime::block_on(
                              liquid::build::verify_provenance(*repository, std::string{target}));
    } else if (!parsed) {
        report = liquid::runtime::block_on(
            liquid::build::verify_wrapped_provenance(root, std::filesystem::path{target}));
    } else {
        return fail(liquid::error{liquid::errc::not_found,
                                  "a result digest requires a liquid repository",
                                  root.string()});
    }
    if (!report) {
        return fail(report.error());
    }
    if (!report->verified) {
        std::println(stderr, "error: invalid provenance: {}", report->reason);
        return 1;
    }
    auto output = liquid::build::provenance_json(*report, dsse);
    if (!output) {
        return fail(output.error());
    }
    std::println("{}", *output);
    return 0;
}

} // namespace liquid_cli
