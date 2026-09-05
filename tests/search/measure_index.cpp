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
    return 0;
}
