// SPDX-License-Identifier: MIT

#include <filesystem>
#include <optional>
#include <print>
#include <string>
#include <string_view>

#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {}", message);
    return 2;
}

} // namespace

auto sbom(int argc, char** argv) -> int {
    auto target = std::string_view{};
    auto format = std::optional<liquid::sbom_format>{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--spdx" || argument == "--cyclonedx") {
            const auto selected =
                argument == "--spdx" ? liquid::sbom_format::spdx : liquid::sbom_format::cyclonedx;
            if (format && *format != selected) {
                return usage("sbom accepts exactly one of --spdx or --cyclonedx");
            }
            format = selected;
            continue;
        }
        if (argument.starts_with('-')) {
            return usage("sbom accepts a target and --spdx or --cyclonedx");
        }
        if (!target.empty()) {
            return usage("sbom accepts one target");
        }
        target = argument;
    }
    if (target.empty() || !format) {
        return usage("sbom requires a target and --spdx or --cyclonedx");
    }
    const auto root = std::filesystem::current_path();
    const auto artifact = std::filesystem::path{target};
    auto report = liquid::result<liquid::sbom_report>{std::unexpected(
        liquid::error{liquid::errc::not_found, "SBOM target not found", std::string{target}})};
    if (artifact.is_absolute() && std::filesystem::is_regular_file(artifact)) {
        report = liquid::runtime::block_on(liquid::build::infer_sbom(artifact, *format));
    } else if (std::filesystem::exists(root / "LIQUID_REPO")) {
        auto repository = liquid::runtime::block_on(liquid::repository::open(root));
        if (!repository) {
            return fail(repository.error());
        }
        report = liquid::runtime::block_on(
            liquid::build::generate_sbom(*repository, std::string{target}, *format));
    } else {
        report = liquid::runtime::block_on(
            liquid::build::generate_wrapped_sbom(root, std::filesystem::path{target}, *format));
    }
    if (!report && report.error().code == liquid::errc::not_found &&
        std::filesystem::is_regular_file(root / target)) {
        report = liquid::runtime::block_on(liquid::build::infer_sbom(root / target, *format));
    }
    if (!report) {
        return fail(report.error());
    }
    if (!report->verified) {
        std::println(stderr, "error: invalid SBOM: {}", report->reason);
        return 1;
    }
    std::println("{}", report->document);
    return 0;
}

} // namespace liquid_cli
