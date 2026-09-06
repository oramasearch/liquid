#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <print>
#include <string_view>
#include <vector>

#include "common/grammar_registry.hpp"
#include "common/syntax.hpp"

namespace {

auto sample(std::string_view language) -> std::vector<std::uint8_t> {
    const auto path = std::filesystem::path{LIQUID_GRAMMAR_FIXTURES} / language / "sample";
    auto stream = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{stream}, std::istreambuf_iterator<char>{}};
}

} // namespace

auto main() -> int {
    constexpr auto iterations = 100;
    for (const auto& grammar : liquid_frontend::available_grammars()) {
        const auto source = sample(grammar.language);
        const auto started = std::chrono::steady_clock::now();
        for (auto iteration = 0; iteration < iterations; ++iteration) {
            auto has_error = liquid_frontend::parse_has_error(grammar.language, source);
            if (!has_error) {
                std::println(stderr, "{}: {}", grammar.language, has_error.error().message);
                return 1;
            }
            if (*has_error) {
                std::println(stderr, "{}: sample contains an ERROR node", grammar.language);
                return 1;
            }
        }
        const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now() - started);
        std::println("{}\t{}", grammar.language, elapsed.count() / iterations);
    }
    return 0;
}
