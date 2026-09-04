#include <filesystem>
#include <print>
#include <string>

#include "liquid/git.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

} // namespace

auto git_import(int argc, char** argv) -> int {
    if (argc != 2 || std::string_view{argv[0]} != "git") {
        std::println(stderr, "error: usage: import git <path> ()");
        return 2;
    }
    const auto path = std::filesystem::path{argv[1]};
    auto repository = liquid::runtime::block_on(liquid::repository::open(path));
    if (!repository) {
        repository = liquid::runtime::block_on(liquid::repository::create(path));
    }
    if (!repository) {
        return fail(repository.error());
    }
    const auto report = liquid::runtime::block_on(liquid::import_git(*repository, path));
    if (!report) {
        return fail(report.error());
    }
    std::println("commits={} blobs={} skipped={} peak_rss_bytes={}",
                 report->commits,
                 report->blobs,
                 report->skipped_commits,
                 report->peak_rss_bytes);
    return 0;
}

auto git_export(int argc, char** argv) -> int {
    if ((argc != 2 && argc != 3) || std::string_view{argv[0]} != "git") {
        std::println(stderr, "error: usage: export git <path> [ref] ()");
        return 2;
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    const auto ref = argc == 3 ? std::string{argv[2]} : "main";
    const auto report = liquid::runtime::block_on(
        liquid::export_git(*repository, std::filesystem::path{argv[1]}, ref));
    if (!report) {
        return fail(report.error());
    }
    std::println("commits={}", report->commits);
    for (const auto& degradation : report->degradations) {
        std::println("degraded: {}", degradation);
    }
    return 0;
}

} // namespace liquid_cli
