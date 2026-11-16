// SPDX-License-Identifier: MIT

#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/projection.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 5) {
        std::println(
            stderr,
            "usage: installer_fixture <macos-pkg|macos-dmg|msix> <root> <output> <repository>");
        return 2;
    }
    auto target = liquid::projection_target::msix;
    if (std::string_view{argv[1]} == "macos-pkg") {
        target = liquid::projection_target::macos_pkg;
    } else if (std::string_view{argv[1]} == "macos-dmg") {
        target = liquid::projection_target::macos_dmg;
    } else if (std::string_view{argv[1]} != "msix") {
        std::println(stderr, "unsupported installer target: {}", argv[1]);
        return 2;
    }
    auto repository = liquid::runtime::block_on(liquid::repository::create(argv[4]));
    if (!repository) {
        return fail(repository.error());
    }
    auto imported = liquid::runtime::block_on(repository->import_tree(argv[2]));
    if (!imported) {
        return fail(imported.error());
    }
    auto root = liquid::runtime::block_on(repository->root_digest());
    if (!root) {
        return fail(root.error());
    }
    const auto windows = target == liquid::projection_target::msix;
    auto request = liquid::projection_request{
        .sources = {{*root}},
        .metadata = {.name = "liquid-installer-test",
                     .version = "1.0.0",
                     .architecture = windows ? "x86_64" : "arm64",
                     .description = "Liquid platform installer fixture",
                     .identifier = windows ? "OramaSearch.LiquidInstallerTest"
                                           : "dev.oramasearch.liquid.installer-test",
                     .publisher = windows ? "CN=Liquid Installer Test" : "",
                     .display_name = "Liquid Installer Test",
                     .executable = windows ? "bin/liquid-installer-fixture.exe"
                                           : "bin/liquid-installer-fixture"},
        .target = target,
        .options = {.output_directory = argv[3], .source_date_epoch = 1'700'000'000}};
    auto report = liquid::runtime::block_on(liquid::project(*repository, std::move(request)));
    if (!report) {
        return fail(report.error());
    }
    if (report->artifacts.size() != 1 || report->signing_requests.size() != 1) {
        std::println(stderr, "projection did not emit one artifact and one signing request");
        return 1;
    }
    std::println("artifact={}", report->artifacts.front().path.string());
    std::println("digest={}", liquid::to_string(report->artifacts.front().digest));
    std::println("request={}", report->signing_requests.front().path.string());
    return 0;
}
