#include <charconv>
#include <cstdint>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/daemon.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

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

} // namespace

auto search(int argc, char** argv) -> int {
    auto query = std::string{};
    auto options = liquid::search_options{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--ref") {
            if (++index == argc || options.ref) {
                return usage("search <query> [--ref <name>] [--limit <count>]");
            }
            options.ref = argv[index];
            continue;
        }
        if (argument == "--limit") {
            if (++index == argc) {
                return usage("search <query> [--ref <name>] [--limit <count>]");
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
        return usage("search <query> [--ref <name>] [--limit <count>]");
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
    auto hits = liquid::runtime::block_on(repository->search(std::move(query), std::move(options)));
    if (!hits) {
        return fail(hits.error());
    }
    for (const auto& hit : *hits) {
        std::println("{:.6f}\t{}\t{}", hit.score, hit.path, hit.snippet);
    }
    return 0;
}

} // namespace liquid_cli
