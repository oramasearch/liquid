// SPDX-License-Identifier: MIT

#include <filesystem>
#include <format>
#include <optional>
#include <print>
#include <string>
#include <string_view>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

#include "cli/signer.hpp"

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

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '\"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

} // namespace

auto path_history(int argc, char** argv) -> int {
    if (argc != 1 && argc != 3) {
        return usage("history <path> [--restore <change>]");
    }
    if (argc == 3 && std::string_view{argv[1]} != "--restore") {
        return usage("history <path> [--restore <change>]");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    if (argc == 3) {
        auto parsed = liquid::parse_digest(argv[2]);
        if (!parsed) {
            return fail(parsed.error());
        }
        auto restored = liquid::runtime::block_on(repository->restore(argv[0], {*parsed}));
        if (!restored) {
            return fail(restored.error());
        }
        std::println("restored {} from {}", argv[0], liquid::to_string(*parsed));
        return 0;
    }
    auto entries = liquid::runtime::block_on(repository->history(argv[0]));
    if (!entries) {
        return fail(entries.error());
    }
    for (const auto& entry : *entries) {
        std::println("{} {} {} {} {} {}",
                     liquid::to_string(entry.change.value),
                     entry.at.time_since_epoch().count(),
                     shown_signer(entry.by, entry.identity_verified),
                     entry.deleted ? "deleted" : "version",
                     entry.path,
                     entry.message);
    }
    return 0;
}

auto history_operations(int argc, char** argv) -> int {
    auto json = false;
    if (argc == 1 && std::string_view{argv[0]} == "--json") {
        json = true;
    } else if (argc != 0) {
        return usage("ops accepts only --json");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    auto operations = liquid::runtime::block_on(repository->operations());
    if (!operations) {
        return fail(operations.error());
    }
    if (json) {
        std::print("[");
        for (std::size_t index = 0; index < operations->size(); ++index) {
            const auto& operation = (*operations)[index];
            std::print("{}{{\"id\":{},\"kind\":{},\"from\":{},\"to\":{},\"actor\":{},"
                       "\"identity_verified\":{},\"at\":{},\"session\":{}}}",
                       index == 0 ? "" : ",",
                       json_string(liquid::to_string(operation.id.value)),
                       json_string(operation.kind),
                       json_string(operation.from),
                       json_string(operation.to),
                       json_string(shown_signer(operation.by, operation.identity_verified)),
                       operation.identity_verified,
                       operation.at.time_since_epoch().count(),
                       json_string(liquid::to_string(operation.session)));
        }
        std::println("]");
    } else {
        for (const auto& operation : *operations) {
            std::println("{} {} {} {} {} {} {}",
                         liquid::to_string(operation.id.value),
                         operation.kind,
                         operation.from,
                         operation.to,
                         shown_signer(operation.by, operation.identity_verified),
                         operation.at.time_since_epoch().count(),
                         liquid::to_string(operation.session));
        }
    }
    return 0;
}

auto history_undo(int argc, char** argv) -> int {
    auto operation = std::optional<liquid::op_id>{};
    auto session = std::optional<liquid::session_id>{};
    auto options = liquid::undo_options{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--session" && index + 1 < argc) {
            auto parsed = liquid::parse_session_id(argv[++index]);
            if (!parsed) {
                return fail(parsed.error());
            }
            session = *parsed;
        } else if (argument == "--path" && index + 1 < argc) {
            options.paths.emplace_back(argv[++index]);
        } else if (argument == "--drip" && index + 1 < argc) {
            auto parsed = liquid::parse_digest(argv[++index]);
            if (!parsed) {
                return fail(parsed.error());
            }
            options.drips.push_back({*parsed});
        } else if (!argument.starts_with("--") && !operation) {
            auto parsed = liquid::parse_digest(argument);
            if (!parsed) {
                return fail(parsed.error());
            }
            operation = liquid::op_id{*parsed};
        } else {
            return usage("undo [op-id] | undo --session <id> [--path <path>]... [--drip <id>]...");
        }
    }
    if (session && operation) {
        return usage("undo cannot combine an operation id with --session");
    }
    if (!session && (!options.paths.empty() || !options.drips.empty())) {
        return usage("--path and --drip require --session");
    }
    auto repository = open_repository();
    if (!repository) {
        return fail(repository.error());
    }
    if (!session && !operation) {
        auto operations = liquid::runtime::block_on(repository->operations());
        if (!operations) {
            return fail(operations.error());
        }
        if (operations->empty()) {
            return fail({liquid::errc::not_found, "repository has no operations", {}});
        }
        operation = operations->front().id;
    }
    auto undone = session ? liquid::runtime::block_on(repository->undo_session(*session, options))
                          : liquid::runtime::block_on(repository->undo(*operation));
    if (!undone) {
        return fail(undone.error());
    }
    for (const auto& applied : undone->applied) {
        std::println("applied {}", liquid::to_string(applied.value));
    }
    for (const auto& skipped : undone->skipped) {
        std::println(
            "skipped {} {} {}", liquid::to_string(skipped.op.value), skipped.path, skipped.reason);
    }
    return 0;
}

} // namespace liquid_cli
