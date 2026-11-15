#include "cli/lake.hpp"

#include <cstdio>
#include <filesystem>
#include <format>
#include <iostream>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: lake add <name> <url> | lake list [--json] | lake use <name> "
                 "[<repository>] | lake remove <name> | lake login <name> | lake logout <name> | "
                 "lake check [<name>] [--json]");
    return 2;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        if (character == '"' || character == '\\') {
            output += '\\';
            output += character;
        } else if (static_cast<unsigned char>(character) < 0x20U) {
            output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
        } else {
            output += character;
        }
    }
    return output + '"';
}

auto source_name(liquid::lake_token_source source) -> std::string_view {
    switch (source) {
    case liquid::lake_token_source::credential_store:
        return "credential_store";
    case liquid::lake_token_source::lake_environment:
        return "lake_environment";
    case liquid::lake_token_source::shared_environment:
        return "shared_environment";
    case liquid::lake_token_source::none:
        break;
    }
    return "none";
}

auto kind_name(liquid::lake_compatibility_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::lake_compatibility_kind::compatible:
        return "compatible";
    case liquid::lake_compatibility_kind::unsupported:
        return "unsupported";
    case liquid::lake_compatibility_kind::unknown:
        break;
    }
    return "unknown";
}

auto open_registry() -> liquid::result<liquid::lake_registry> {
    return liquid::runtime::block_on(liquid::lake_registry::open());
}

// The repository in the current directory, when there is one.
auto current_repository() -> std::optional<liquid::repository> {
    auto opened =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!opened) {
        return std::nullopt;
    }
    return std::move(*opened);
}

auto add(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    if (argc != 2) {
        return usage();
    }
    auto added = liquid::runtime::block_on(lakes.add(argv[0], argv[1]));
    if (!added) {
        return fail(added.error());
    }
    std::println("added lake {} {}", added->name, added->url);
    return 0;
}

auto list(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    const auto json = argc == 1 && std::string_view{argv[0]} == "--json";
    if (argc != 0 && !json) {
        return usage();
    }
    auto current = std::string{};
    if (auto repository = current_repository()) {
        auto recorded = liquid::runtime::block_on(liquid::read_repository_lake(*repository));
        if (recorded && *recorded) {
            current = (*recorded)->lake;
        }
    }
    if (json) {
        std::print("[");
    } else if (lakes.list().empty()) {
        std::println("no lakes: add one with `liquid lake add <name> <url>`");
        return 0;
    }
    auto first = true;
    for (const auto& entry : lakes.list()) {
        auto token = liquid::runtime::block_on(lakes.token(entry.name));
        const auto source = token ? token->source : liquid::lake_token_source::none;
        if (json) {
            std::print("{}{{\"name\":{},\"url\":{},\"token\":{},\"current\":{}}}",
                       first ? "" : ",",
                       json_string(entry.name),
                       json_string(entry.url),
                       json_string(source_name(source)),
                       entry.name == current ? "true" : "false");
        } else {
            std::println("{} {} {} token={}",
                         entry.name == current ? '*' : ' ',
                         entry.name,
                         entry.url,
                         source_name(source));
        }
        first = false;
    }
    if (json) {
        std::println("]");
    }
    return 0;
}

auto use(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    if (argc != 1 && argc != 2) {
        return usage();
    }
    auto entry = lakes.find(argv[0]);
    if (!entry) {
        return fail(liquid::error{liquid::errc::not_found,
                                  "no lake has this name: add it with `liquid lake add " +
                                      std::string{argv[0]} + " <url>`",
                                  argv[0]});
    }
    auto opened =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!opened) {
        return fail(opened.error());
    }
    auto recorded = liquid::runtime::block_on(liquid::read_repository_lake(*opened));
    if (!recorded) {
        return fail(recorded.error());
    }
    auto path = std::string{};
    if (argc == 2) {
        auto resolved = liquid::repository_path_for(*entry, argv[1]);
        if (!resolved) {
            return fail(resolved.error());
        }
        path = std::move(*resolved);
    } else if (*recorded) {
        path = (*recorded)->repository;
    }
    if (path.empty()) {
        return fail(liquid::error{liquid::errc::not_configured,
                                  "name the repository on this lake: `liquid lake use " +
                                      entry->name + " <repository>`",
                                  entry->name});
    }
    auto written = liquid::runtime::block_on(
        liquid::write_repository_lake(*opened, liquid::repository_lake{entry->name, path}));
    if (!written) {
        return fail(written.error());
    }
    std::println("using lake {} {}", entry->name, entry->url + path);
    return 0;
}

auto remove(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    if (argc != 1) {
        return usage();
    }
    auto removed = liquid::runtime::block_on(lakes.remove(argv[0]));
    if (!removed) {
        return fail(removed.error());
    }
    std::println("removed lake {}", argv[0]);
    return 0;
}

auto login(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    if (argc != 1) {
        return usage();
    }
    if (!lakes.find(argv[0])) {
        return fail(liquid::error{liquid::errc::not_found, "no lake has this name", argv[0]});
    }
    // The token is read from standard input so that it never appears in an argument list.
    auto token = std::string{};
    std::getline(std::cin, token);
    while (!token.empty() && (token.back() == '\r' || token.back() == ' ')) {
        token.pop_back();
    }
    auto stored = liquid::runtime::block_on(lakes.set_token(argv[0], std::move(token)));
    if (!stored) {
        return fail(stored.error());
    }
    std::println("stored the token for lake {}", argv[0]);
    return 0;
}

auto logout(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    if (argc != 1) {
        return usage();
    }
    auto cleared = liquid::runtime::block_on(lakes.clear_token(argv[0]));
    if (!cleared) {
        return fail(cleared.error());
    }
    std::println("removed the stored token for lake {}", argv[0]);
    return 0;
}

auto check(liquid::lake_registry& lakes, int argc, char** argv) -> int {
    auto json = false;
    auto name = std::string{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json" && !json) {
            json = true;
        } else if (name.empty() && !argument.starts_with('-')) {
            name = argument;
        } else {
            return usage();
        }
    }
    if (name.empty()) {
        if (auto repository = current_repository()) {
            auto recorded = liquid::runtime::block_on(liquid::read_repository_lake(*repository));
            if (!recorded) {
                return fail(recorded.error());
            }
            if (*recorded) {
                name = (*recorded)->lake;
            }
        }
    }
    if (name.empty()) {
        return fail(liquid::error{
            liquid::errc::not_configured, "name a lake: `liquid lake check <name>`", "lake"});
    }
    auto entry = lakes.find(name);
    if (!entry) {
        return fail(liquid::error{liquid::errc::not_found, "no lake has this name", name});
    }
    auto target = liquid::lake_target{.named = entry, .base_url = entry->url};
    auto decided = liquid::runtime::block_on(liquid::check_lake(target));
    if (!decided) {
        return fail(decided.error());
    }
    if (json) {
        std::println("{{\"lake\":{},\"url\":{},\"compatibility\":{},\"message\":{},"
                     "\"version\":{},\"revision\":{}}}",
                     json_string(entry->name),
                     json_string(entry->url),
                     json_string(kind_name(decided->kind)),
                     json_string(decided->message),
                     json_string(decided->info.version),
                     json_string(decided->info.revision));
    } else {
        std::println("{}: {}", kind_name(decided->kind), decided->message);
    }
    return decided->kind == liquid::lake_compatibility_kind::unsupported ? 1 : 0;
}

} // namespace

auto prepare_lake(liquid::repository* subject, std::string argument)
    -> liquid::result<liquid::lake_target> {
    auto lakes = open_registry();
    if (!lakes) {
        return std::unexpected(lakes.error());
    }
    auto target = liquid::runtime::block_on(
        liquid::resolve_lake_target(*lakes, subject, std::move(argument)));
    if (!target) {
        return std::unexpected(target.error());
    }
    auto decided = liquid::runtime::block_on(liquid::require_compatible_lake(*target));
    if (!decided) {
        return std::unexpected(decided.error());
    }
    if (decided->kind == liquid::lake_compatibility_kind::unknown) {
        std::println(stderr, "warning: {}", decided->message);
    }
    return target;
}

auto remember_lake(liquid::repository& subject, const liquid::lake_target& target) -> void {
    if (!target.named) {
        return;
    }
    auto recorded = liquid::runtime::block_on(liquid::read_repository_lake(subject));
    if (!recorded || *recorded) {
        return;
    }
    auto path = liquid::repository_path_for(*target.named, target.repository_url);
    if (!path || path->empty()) {
        return;
    }
    auto written = liquid::runtime::block_on(liquid::write_repository_lake(
        subject, liquid::repository_lake{target.named->name, std::move(*path)}));
    if (!written) {
        std::println(stderr, "warning: could not record the lake: {}", written.error().message);
    }
}

auto lake_command(int argc, char** argv) -> int {
    if (argc == 0) {
        return usage();
    }
    auto lakes = open_registry();
    if (!lakes) {
        return fail(lakes.error());
    }
    const auto verb = std::string_view{argv[0]};
    if (verb == "add") {
        return add(*lakes, argc - 1, argv + 1);
    }
    if (verb == "list") {
        return list(*lakes, argc - 1, argv + 1);
    }
    if (verb == "use") {
        return use(*lakes, argc - 1, argv + 1);
    }
    if (verb == "remove") {
        return remove(*lakes, argc - 1, argv + 1);
    }
    if (verb == "login") {
        return login(*lakes, argc - 1, argv + 1);
    }
    if (verb == "logout") {
        return logout(*lakes, argc - 1, argv + 1);
    }
    if (verb == "check") {
        return check(*lakes, argc - 1, argv + 1);
    }
    return usage();
}

} // namespace liquid_cli
