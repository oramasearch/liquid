#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <print>
#include <string_view>
#include <vector>

#include "search/index.hpp"

namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto elapsed_us(auto&& callable) -> liquid::result<std::uint64_t> {
    const auto started = std::chrono::steady_clock::now();
    auto result = callable();
    if (!result) {
        return std::unexpected(result.error());
    }
    return static_cast<std::uint64_t>(std::chrono::duration_cast<std::chrono::microseconds>(
                                          std::chrono::steady_clock::now() - started)
                                          .count());
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::println(stderr, "usage: liquid-search-measure <checkout> <index-directory>");
        return 2;
    }
    const auto checkout = std::filesystem::path{argv[1]};
    const auto index = std::filesystem::path{argv[2]};
    const auto build_started = std::chrono::steady_clock::now();
    auto published = liquid::search_detail::publish_checkout(checkout, index, "measurement");
    if (!published) {
        return fail(published.error());
    }
    const auto build_elapsed = std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now() - build_started);
    auto statistics = liquid::search_detail::statistics(index, "working");
    if (!statistics) {
        return fail(statistics.error());
    }

    constexpr auto repetitions = std::uint64_t{2};
    const auto queries = std::vector<std::string_view>{
        "repository", "runtime", "search", "include", "\"query graph\""};
    const auto query_started = std::chrono::steady_clock::now();
    for (auto repetition = std::uint64_t{}; repetition != repetitions; ++repetition) {
        for (const auto value : queries) {
            auto hits = liquid::search_detail::query(index, "working", std::string{value}, 20);
            if (!hits) {
                return fail(hits.error());
            }
        }
    }
    const auto query_elapsed = std::chrono::duration_cast<std::chrono::microseconds>(
        std::chrono::steady_clock::now() - query_started);
    const auto iterations = repetitions * queries.size();
    std::println("search index build: files={} unique_blobs={} bytes={} elapsed_ms={}",
                 statistics->paths,
                 statistics->unique_blobs,
                 statistics->bytes,
                 build_elapsed.count());
    std::println("search query mean: iterations={} mean_us={}",
                 iterations,
                 query_elapsed.count() / static_cast<std::int64_t>(iterations));
    auto documents = liquid::search_detail::checkout_documents(checkout);
    if (!documents) {
        return fail(documents.error());
    }
    auto publication_documents = liquid::search_detail::checkout_documents(checkout, true);
    if (!publication_documents) {
        return fail(publication_documents.error());
    }
    const auto scopes = std::vector<std::size_t>{10, 50, 100, 250, 500, 1000, 2000, 4000};
    for (const auto requested : scopes) {
        const auto count = std::min(requested, documents->size());
        if (count == 0) {
            continue;
        }
        auto subset = std::vector<liquid::search_detail::document>{documents->begin(),
                                                                   documents->begin() + count};
        auto publication_subset = std::vector<liquid::search_detail::document>{
            publication_documents->begin(), publication_documents->begin() + count};
        auto scoped = liquid::search_detail::publish_full(
            index, "working", "scope-" + std::to_string(count), publication_subset);
        if (!scoped) {
            return fail(scoped.error());
        }
        auto literal_scan = elapsed_us(
            [&] { return liquid::search_detail::scan(subset, "\"query graph\"", 1000); });
        auto literal_index = elapsed_us([&] {
            return liquid::search_detail::query(
                index, "working", "\"query graph\"", 1000, liquid::search_executor::trigram);
        });
        auto regex_scan = elapsed_us(
            [&] { return liquid::search_detail::scan(subset, "/query graph.*/", 1000); });
        auto regex_index = elapsed_us([&] {
            return liquid::search_detail::query(
                index, "working", "/query graph.*/", 1000, liquid::search_executor::trigram);
        });
        auto selectivity = liquid::search_detail::selectivity(index, "working", "\"query graph\"");
        if (!literal_scan || !literal_index || !regex_scan || !regex_index || !selectivity) {
            return fail(!literal_scan    ? literal_scan.error()
                        : !literal_index ? literal_index.error()
                        : !regex_scan    ? regex_scan.error()
                        : !regex_index   ? regex_index.error()
                                         : selectivity.error());
        }
        std::println(
            "search crossover: files={} literal_scan_us={} literal_index_us={} regex_scan_us={} "
            "regex_index_us={} selectivity={:.6f}",
            count,
            *literal_scan,
            *literal_index,
            *regex_scan,
            *regex_index,
            *selectivity);
        if (count == documents->size()) {
            break;
        }
    }
    return 0;
}
