#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/install.hpp"

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

auto root_option(int argc, char** argv, bool& list) -> liquid::result<std::filesystem::path> {
    auto root = std::filesystem::path{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--list") {
            list = true;
            continue;
        }
        if (argument == "--root") {
            if (++index == argc) {
                return std::unexpected(liquid::error{
                    liquid::errc::invalid_argument, "--root requires a directory", "install"});
            }
            root = argv[index];
        }
    }
    if (root.empty()) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, "--root is required", "install"});
    }
    return root;
}

} // namespace

auto install(int argc, char** argv) -> int {
    auto list = false;
    auto root = root_option(argc, argv, list);
    if (!root) {
        return fail(root.error());
    }
    if (list) {
        for (auto index = 0; index < argc; ++index) {
            const auto argument = std::string_view{argv[index]};
            if (argument != "--list" && argument != "--root" &&
                (index == 0 || std::string_view{argv[index - 1]} != "--root")) {
                return usage("install --list --root <dir> accepts no package");
            }
        }
        auto generations = liquid::runtime::block_on(liquid::installation::list(*root));
        if (!generations) {
            return fail(generations.error());
        }
        for (const auto& generation : *generations) {
            std::println("generation={} current={} path={}",
                         generation.number,
                         generation.current ? "true" : "false",
                         generation.path.string());
        }
        return 0;
    }
    if (argc < 1 || std::string_view{argv[0]}.starts_with("--")) {
        return usage("install <package|install-tree> --root <dir>");
    }
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--root") {
            ++index;
            continue;
        }
        return usage("install received an unknown option");
    }
    auto report = liquid::runtime::block_on(liquid::installation::install(argv[0], *root));
    if (!report) {
        return fail(report.error());
    }
    std::println("generation={} current=true files={} linked={}",
                 report->generation.number,
                 report->files,
                 report->linked);
    return 0;
}

auto rollback(int argc, char** argv) -> int {
    auto list = false;
    auto root = root_option(argc, argv, list);
    if (!root) {
        return fail(root.error());
    }
    if (list || argc != 2 || std::string_view{argv[0]} != "--root") {
        return usage("rollback --root <dir>");
    }
    auto generation = liquid::runtime::block_on(liquid::installation::rollback(*root));
    if (!generation) {
        return fail(generation.error());
    }
    std::println(
        "generation={} current=true path={}", generation->number, generation->path.string());
    return 0;
}

} // namespace liquid_cli
