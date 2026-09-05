#include <cstdlib>
#include <filesystem>
#include <print>
#include <string_view>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/sync.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view command) -> int {
    std::println(stderr, "error: usage: {} <url> ()", command);
    return 2;
}

auto run(int argc, char** argv, bool push) -> int {
    if (argc != 1) {
        return usage(push ? "push" : "pull");
    }
    auto opened =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!opened) {
        return fail(opened.error());
    }
    auto options = liquid::sync_endpoint_options{};
    if (const auto* token = std::getenv("LIQUID_TOKEN")) {
        options.bearer_token = token;
    }
    try {
        auto endpoint = liquid::sync_endpoint{argv[0], std::move(options)};
        auto synced = push ? liquid::runtime::block_on(endpoint.push(*opened))
                           : liquid::runtime::block_on(endpoint.pull(*opened));
        if (!synced) {
            return fail(synced.error());
        }
        std::println("metadata_items={}", synced->metadata_items);
        std::println("objects_sent={}", synced->sent.objects);
        std::println("bytes_sent={}", synced->sent.bytes);
        std::println("objects_received={}", synced->received.objects);
        std::println("bytes_received={}", synced->received.bytes);
        std::println("cursor={}", synced->cursor.token);
        return 0;
    } catch (const std::exception& exception) {
        return fail(liquid::error{liquid::errc::invalid_argument, exception.what(), "sync URL"});
    }
}

} // namespace

auto sync_push(int argc, char** argv) -> int {
    return run(argc, argv, true);
}

auto sync_pull(int argc, char** argv) -> int {
    return run(argc, argv, false);
}

} // namespace liquid_cli
