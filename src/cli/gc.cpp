#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string_view>

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

auto garbage_collect(int argc, char** argv) -> int {
    auto options = liquid::collection_options{};
    auto path = std::filesystem::current_path();
    auto path_seen = false;
    auto json = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--dry-run") {
            options.dry_run = true;
        } else if (argument == "--json") {
            json = true;
        } else if (argument == "--explain") {
            options.explain = true;
        } else if (argument == "--compact") {
            options.compact_only = true;
        } else if (argument == "--compact-below") {
            if (++index == argc) {
                return usage("gc --compact-below requires a fraction");
            }
            char* end = nullptr;
            options.compact_below = std::strtod(argv[index], &end);
            if (end == argv[index] || *end != '\0' || !std::isfinite(options.compact_below) ||
                options.compact_below < 0.0 || options.compact_below > 1.0) {
                return usage("gc --compact-below must be a fraction from zero to one");
            }
        } else if (argument.starts_with('-')) {
            return usage(std::format("unknown gc option '{}'", argument));
        } else if (path_seen) {
            return usage("gc accepts at most one repository path");
        } else {
            path = argv[index];
            path_seen = true;
        }
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(path));
    if (!repository) {
        return fail(repository.error());
    }
    auto report = liquid::runtime::block_on(repository->collect(options));
    if (!report) {
        return fail(report.error());
    }
    if (json) {
        std::println(
            "{{\"epoch\":{},\"roots\":{},\"live_objects\":{},\"live_chunks\":{},\"dead_chunks\":{},"
            "\"bytes_reclaimed\":{},\"packs_deleted\":{},\"packs_compacted\":{}}}",
            report->epoch,
            report->roots,
            report->live_objects,
            report->live_chunks,
            report->dead_chunks,
            report->bytes_reclaimed,
            report->packs_deleted,
            report->packs_compacted);
        return 0;
    }
    std::println("epoch={} roots={} live_objects={} live_chunks={} dead_chunks={}",
                 report->epoch,
                 report->roots,
                 report->live_objects,
                 report->live_chunks,
                 report->dead_chunks);
    std::println("packs_deleted={} bytes_reclaimed={} partial_pack_bytes={} packs_compacted={}",
                 report->packs_deleted,
                 report->bytes_reclaimed,
                 report->bytes_reclaimable_in_partially_dead_packs,
                 report->packs_compacted);
    std::println("compaction_bytes_reclaimed={} compaction_bytes_written={}",
                 report->bytes_reclaimed_by_compaction,
                 report->bytes_written_by_compaction);
    for (const auto& [rule, count] : report->retained_by_rule) {
        std::println("retained_by_rule={} count={}", rule, count);
    }
    if (options.explain) {
        for (const auto& retained : report->retained_chunks) {
            std::println("{} retained: {}", liquid::to_string(retained.id), retained.reason);
        }
    }
    return 0;
}

auto repack(int argc, char** argv) -> int {
    if (argc > 1) {
        return usage("repack accepts at most one repository path");
    }
    const auto path = argc == 1 ? std::filesystem::path{argv[0]} : std::filesystem::current_path();
    auto repository = liquid::runtime::block_on(liquid::repository::open(path));
    if (!repository) {
        return fail(repository.error());
    }
    auto options = liquid::collection_options{};
    options.compact_only = true;
    options.repack = true;
    auto report = liquid::runtime::block_on(repository->collect(options));
    if (!report) {
        return fail(report.error());
    }
    std::println("packs_rewritten={} deltas_rebased={} bytes_written={}",
                 report->packs_compacted,
                 report->deltas_rebased,
                 report->bytes_written_by_compaction);
    return 0;
}

} // namespace liquid_cli
