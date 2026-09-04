#include <cmath>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <print>
#include <span>
#include <string_view>

#include "liquid/liquid.hpp"

namespace {

int usage() {
    std::println("liquid — a version control system, build system, package manager,");
    std::println("artifact service and runtime in one program.");
    std::println("");
    std::println("usage: liquid <command> [options]");
    std::println("");
    std::println("commands:");
    std::println("  version    print the version");
    std::println("  hello      say hello (proves the toolchain end to end)");
    std::println("  gc         collect an on-disk store (path defaults to current directory)");
    return 2;
}

int collect(std::span<char*> args) {
    liquid::collection_options options;
    std::filesystem::path path = std::filesystem::current_path();
    bool path_seen = false;
    for (std::size_t index = 2; index < args.size(); ++index) {
        const std::string_view argument = args[index];
        if (argument == "--explain") {
            options.explain = true;
        } else if (argument == "--compact") {
            options.compact_only = true;
        } else if (argument == "--compact-below") {
            if (++index == args.size()) {
                std::println(stderr, "gc: --compact-below requires a fraction");
                return 2;
            }
            char* end = nullptr;
            options.compact_below = std::strtod(args[index], &end);
            if (end == args[index] || *end != '\0' || !std::isfinite(options.compact_below) ||
                options.compact_below < 0.0 || options.compact_below > 1.0) {
                std::println(stderr, "gc: --compact-below must be a fraction from zero to one");
                return 2;
            }
        } else if (argument.starts_with("-")) {
            std::println(stderr, "unknown gc option: {}", argument);
            return 2;
        } else if (path_seen) {
            std::println(stderr, "gc accepts at most one store path");
            return 2;
        } else {
            path = argument;
            path_seen = true;
        }
    }
    auto opened = liquid::runtime::block_on(liquid::store::open(path));
    if (!opened) {
        std::println(stderr, "gc: {}: {}", opened.error().context, opened.error().message);
        return 1;
    }
    auto report = liquid::runtime::block_on(opened->collect(options));
    if (!report) {
        std::println(stderr, "gc: {}: {}", report.error().context, report.error().message);
        return 1;
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
    if (options.explain) {
        for (const auto& retained : report->retained_chunks)
            std::println("{} retained: {}", liquid::to_string(retained.id), retained.reason);
    }
    return 0;
}

} // namespace

int main(int argc, char** argv) {
    const std::span<char*> args(argv, static_cast<std::size_t>(argc));
    if (args.size() < 2)
        return usage();

    const std::string_view cmd = args[1];
    if (cmd == "version" || cmd == "--version") {
        std::println("liquid {}", liquid::version_string());
        return 0;
    }
    if (cmd == "hello") {
        std::println("hello from liquid {} — drip, drip, drip", liquid::version_string());
        return 0;
    }
    if (cmd == "gc")
        return collect(args);
    return usage();
}
