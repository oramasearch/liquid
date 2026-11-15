#include <cstdlib>
#include <filesystem>
#include <print>
#include <string_view>

#include "cli/lake.hpp"
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
    const auto arguments = command == "push" ? "[--atomic] [<lake>|<url>]" : "[<lake>|<url>]";
    std::println(stderr, "error: usage: {} {}", command, arguments);
    return 2;
}

auto run(int argc, char** argv, bool push) -> int {
    auto url = std::string_view{};
    auto atomic = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (push && argument == "--atomic" && !atomic) {
            atomic = true;
            continue;
        }
        if (url.empty()) {
            url = argument;
            continue;
        }
        return usage(push ? "push" : "pull");
    }
    if (!push && atomic) {
        return usage(push ? "push" : "pull");
    }
    auto opened =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!opened) {
        return fail(opened.error());
    }
    // An empty argument is the repository's recorded lake; a name is that lake; a URL is matched
    // to a named lake by host. Only that lake's token is sent (docs/library/lakes.md).
    auto target = prepare_lake(&*opened, std::string{url});
    if (!target) {
        return fail(target.error());
    }
    auto options = liquid::sync_endpoint_options{};
    options.bearer_token = target->token.value;
    options.atomic = atomic;
    try {
        auto endpoint = liquid::sync_endpoint{target->repository_url, std::move(options)};
        auto synced = push ? liquid::runtime::block_on(endpoint.push(*opened))
                           : liquid::runtime::block_on(endpoint.pull(*opened));
        if (!synced) {
            return fail(synced.error());
        }
        remember_lake(*opened, *target);
        if (synced->retry_after_ms != 0) {
            std::println("flow_rejected scope={} retry_after_ms={}",
                         synced->exhausted_scope,
                         synced->retry_after_ms);
            return 1;
        }
        std::println("metadata_items={}", synced->metadata_items);
        std::println("objects_sent={}", synced->sent.objects);
        std::println("bytes_sent={}", synced->sent.bytes);
        std::println("objects_received={}", synced->received.objects);
        std::println("bytes_received={}", synced->received.bytes);
        std::println("cursor={}", synced->cursor.token);
        auto refused = false;
        for (const auto& reference : synced->references) {
            if (reference.outcome == liquid::sync_ref_outcome_kind::applied) {
                std::println("reference={} outcome=applied", reference.name);
                continue;
            }
            refused = true;
            std::println(
                "reference={} outcome=refused reason={}", reference.name, reference.reason);
        }
        return refused ? 1 : 0;
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
