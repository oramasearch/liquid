#include <charconv>
#include <cstdint>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <utility>

#include "liquid/daemon.hpp"
#include "liquid/format.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;
auto print_tier(unsigned level, std::string_view mechanism) -> void;
auto tier_json(unsigned level, std::string_view mechanism) -> std::string;

namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {}", message);
    return 2;
}

auto parse_limit(std::string_view text) -> liquid::result<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), output);
    if (code != std::errc{} || end != text.data() + text.size() || output == 0 || output > 1000) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "search limit must be between 1 and 1000",
                                             std::string{text}});
    }
    return output;
}

auto executor_name(liquid::search_executor value) -> std::string_view {
    switch (value) {
    case liquid::search_executor::bm25f:
        return "bm25f";
    case liquid::search_executor::trigram:
        return "trigram";
    case liquid::search_executor::scan:
        return "scan";
    }
    return "unknown";
}

auto query_kind_name(liquid::search_query_kind value) -> std::string_view {
    switch (value) {
    case liquid::search_query_kind::ranked_text:
        return "ranked_text";
    case liquid::search_query_kind::literal:
        return "literal";
    case liquid::search_query_kind::regex_with_literal_core:
        return "regex_with_literal_core";
    case liquid::search_query_kind::regex_without_literal_core:
        return "regex_without_literal_core";
    }
    return "unknown";
}

auto print_plan(const liquid::search_plan& plan) -> void {
    std::println("{{");
    std::println("  \"tier\": {},", tier_json(0, "chunking"));
    std::println("  \"executor\": \"{}\",", executor_name(plan.executor));
    std::println("  \"query_kind\": \"{}\",", query_kind_name(plan.query_kind));
    std::println("  \"estimated_cost_us\": {},", plan.estimated_cost_us);
    std::println("  \"actual_cost_us\": {},", plan.actual_cost_us);
    std::println("  \"inputs\": {{");
    std::println("    \"working_set_blobs\": {},", plan.working_set_blobs);
    std::println("    \"working_set_bytes\": {},", plan.working_set_bytes);
    std::println("    \"drips_since_index_update\": {},", plan.drips_since_index_update);
    std::println("    \"trigram_selectivity\": {:.6f},", plan.trigram_selectivity);
    std::println("    \"scan_nanoseconds_per_blob\": {},", plan.scan_nanoseconds_per_blob);
    std::println("    \"scan_parallelism\": {}", plan.scan_parallelism);
    std::println("  }},");
    std::println("  \"alternatives\": [");
    for (auto index = std::size_t{}; index != plan.alternatives.size(); ++index) {
        const auto& alternative = plan.alternatives[index];
        std::println("    {{");
        std::println("      \"executor\": \"{}\",", executor_name(alternative.executor));
        std::println("      \"estimated_cost_us\": {},", alternative.estimated_cost_us);
        if (alternative.actual_cost_us) {
            std::println("      \"actual_cost_us\": {},", *alternative.actual_cost_us);
        } else {
            std::println("      \"actual_cost_us\": null,");
        }
        std::println("      \"eligible\": {},", alternative.eligible ? "true" : "false");
        std::println("      \"reason\": \"{}\"", alternative.reason);
        std::println("    }}{}", index + 1 == plan.alternatives.size() ? "" : ",");
    }
    std::println("  ]");
    std::println("}}");
}

auto hit_tier(std::string_view path) -> std::pair<unsigned, std::string> {
    const auto nested = path.rfind("!/");
    if (nested == std::string_view::npos) {
        return {0, "chunking"};
    }
    const auto member = path.substr(nested + 2);
    const auto codecs = liquid::format_registry::global().find_by_extension(member);
    if (codecs.empty()) {
        return {3, "format(opaque)"};
    }
    const auto& descriptor = codecs.front()->descriptor();
    return {static_cast<unsigned>(descriptor.tier), "format(" + descriptor.name + ")"};
}

} // namespace

auto search(int argc, char** argv) -> int {
    auto query = std::string{};
    auto options = liquid::search_options{};
    auto explain = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--explain") {
            explain = true;
            continue;
        }
        if (argument == "--ref") {
            if (++index == argc || options.ref) {
                return usage("search [--explain] <query> [--ref <name>] [--limit <count>]");
            }
            options.ref = argv[index];
            continue;
        }
        if (argument == "--limit") {
            if (++index == argc) {
                return usage("search [--explain] <query> [--ref <name>] [--limit <count>]");
            }
            auto limit = parse_limit(argv[index]);
            if (!limit) {
                return fail(limit.error());
            }
            options.limit = *limit;
            continue;
        }
        if (!query.empty()) {
            return usage("search accepts one query");
        }
        query = argument;
    }
    if (query.empty()) {
        return usage("search [--explain] <query> [--ref <name>] [--limit <count>]");
    }
    const auto checkout = std::filesystem::current_path();
    auto ready = ensure_daemon(checkout);
    if (!ready && ready.error().code != liquid::errc::unsupported) {
        return fail(ready.error());
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(checkout));
    if (!repository) {
        return fail(repository.error());
    }
    if (explain) {
        auto plan = liquid::runtime::block_on(
            repository->plan_search(std::move(query), std::move(options)));
        if (!plan) {
            return fail(plan.error());
        }
        print_plan(*plan);
        return 0;
    }
    auto hits = liquid::runtime::block_on(repository->search(std::move(query), std::move(options)));
    if (!hits) {
        return fail(hits.error());
    }
    for (const auto& hit : *hits) {
        const auto [level, mechanism] = hit_tier(hit.path);
        print_tier(level, mechanism);
        if (hit.score) {
            std::println(
                "{:.6f}\t{}:{}:{}\t{}", *hit.score, hit.path, hit.line, hit.column, hit.snippet);
        } else {
            std::println("-\t{}:{}:{}\t{}", hit.path, hit.line, hit.column, hit.snippet);
        }
    }
    return 0;
}

} // namespace liquid_cli
