// SPDX-License-Identifier: MIT

#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string>

#include "liquid/git.hpp"
#include "liquid/import.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto spack_report(int argc, char** argv) -> int {
    auto into = std::filesystem::current_path();
    auto export_document = std::optional<std::filesystem::path>{};
    auto print_report = false;
    auto json = false;
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--into") {
            if (++index == argc) {
                std::println(stderr, "error: usage: import spack requires a path after --into ()");
                return 2;
            }
            into = argv[index];
        } else if (argument == "--report") {
            print_report = true;
        } else if (argument == "--json") {
            json = true;
            print_report = true;
        } else if (argument.starts_with("--") || export_document) {
            std::println(stderr,
                         "error: usage: import spack <export.json> --into <repository> "
                         "[--report] [--json] | import spack --report [--into <repository>] "
                         "[--json] ()");
            return 2;
        } else {
            export_document = argv[index];
        }
    }
    auto report = liquid::result<liquid::spack_import_report>{
        std::unexpected(liquid::error{liquid::errc::not_found, "import report not loaded", {}})};
    if (export_document) {
        report = liquid::runtime::block_on(liquid::import_spack(*export_document, into));
    } else {
        if (!print_report) {
            std::println(stderr,
                         "error: usage: import spack <export.json> --into <repository> "
                         "[--report] [--json] | import spack --report [--into <repository>] "
                         "[--json] ()");
            return 2;
        }
        const auto path = into / "import-report.json";
        auto input = std::ifstream{path, std::ios::binary};
        if (!input) {
            return fail(
                {liquid::errc::not_found, "Spack import report does not exist", path.string()});
        }
        const auto document = std::string{std::istreambuf_iterator<char>{input}, {}};
        report = liquid::decode_spack_import_report(document);
    }
    if (!report) {
        return fail(report.error());
    }
    if (!print_report) {
        std::println("packages={} translated_fields={} losses={} pinned_patches={}",
                     report->packages,
                     report->translated_fields,
                     report->losses,
                     report->pinned_patches);
        return 0;
    }
    if (json) {
        auto encoded = liquid::spack_import_report_json(*report);
        if (!encoded) {
            return fail(encoded.error());
        }
        std::println("{}", *encoded);
    } else {
        std::print("{}", liquid::spack_import_report_text(*report));
    }
    return 0;
}

} // namespace

auto git_import(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "spack") {
        return spack_report(argc, argv);
    }
    if (argc == 1) {
        auto repository =
            liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
        if (!repository) {
            return fail(repository.error());
        }
        const auto report =
            liquid::runtime::block_on(repository->import_tree(std::filesystem::path{argv[0]}));
        if (!report) {
            return fail(report.error());
        }
        std::println("files_seen={} changed={} bytes_hashed={} bytes_written={} drip={}",
                     report->files_seen,
                     report->changed,
                     report->bytes_hashed,
                     report->bytes_written,
                     report->drip ? liquid::to_string(report->drip->value) : "none");
        return 0;
    }
    if (argc != 2 || std::string_view{argv[0]} != "git") {
        std::println(stderr,
                     "error: usage: import <path> | import git <path> | import spack "
                     "<export.json> --into <repository> ()");
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
    std::println("commits={} blobs={} skipped={} peak_anonymous_bytes={}",
                 report->commits,
                 report->blobs,
                 report->skipped_commits,
                 report->peak_anonymous_bytes);
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
