#include <filesystem>
#include <print>
#include <string_view>
#include <utility>

#include "liquid/hot_update.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {}", message);
    return 2;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(
        liquid::repository::open_build(std::filesystem::current_path()));
}

// liquid#388: who may sign a patch comes from this machine's liquid home, never the repository.
auto signer_trust() -> liquid::result<liquid::hot_update_trust> {
    auto signers = liquid::load_trusted_identities("hot-update-signers");
    if (!signers) {
        return std::unexpected(signers.error());
    }
    return liquid::hot_update_trust{std::move(*signers)};
}

auto classify(int argc, char** argv) -> int {
    const auto json = argc == 3 && std::string_view{argv[2]} == "--json";
    if ((argc != 2 && !json) || (argc == 3 && !json)) {
        return usage("hot-update classify <old> <new> [--json]");
    }
    auto old_result = liquid::parse_digest(argv[0]);
    auto new_result = liquid::parse_digest(argv[1]);
    if (!old_result || !new_result) {
        return fail(old_result ? new_result.error() : old_result.error());
    }
    auto source = open_repository();
    if (!source) {
        return fail(source.error());
    }
    auto classified = liquid::runtime::block_on(liquid::hot_update::classify(
        *source, liquid::result_id{*old_result}, liquid::result_id{*new_result}));
    if (!classified) {
        return fail(classified.error());
    }
    if (json) {
        auto output = liquid::hot_update_plan_json(*classified);
        if (!output) {
            return fail(output.error());
        }
        std::println("{}", *output);
        return 0;
    }
    std::println("tier={}", liquid::to_string(classified->tier));
    std::println("outcome={}", liquid::to_string(classified->outcome));
    for (const auto& symbol : classified->symbols) {
        std::println("symbol={}", symbol);
    }
    for (const auto& evidence : classified->evidence) {
        std::println("evidence={}", liquid::to_string(evidence));
    }
    std::println("reason={}", classified->reason);
    return 0;
}

auto status(int argc, char** argv) -> int {
    const auto json = argc == 2 && std::string_view{argv[1]} == "--json";
    if ((argc != 1 && !json) || (argc == 2 && !json)) {
        return usage("hot-update status <result> [--json]");
    }
    auto target = liquid::parse_digest(argv[0]);
    if (!target) {
        return fail(target.error());
    }
    auto source = open_repository();
    if (!source) {
        return fail(source.error());
    }
    auto trust = signer_trust();
    if (!trust) {
        return fail(trust.error());
    }
    auto composition = liquid::runtime::block_on(
        liquid::hot_update::status(*source, liquid::result_id{*target}, std::move(*trust)));
    if (!composition) {
        return fail(composition.error());
    }
    if (json) {
        auto output = liquid::hot_update_status_json(*composition);
        if (!output) {
            return fail(output.error());
        }
        std::println("{}", *output);
        return 0;
    }
    std::println("artifact={}", liquid::to_string(composition->artifact.value));
    for (std::size_t index = 0; index < composition->patches.size(); ++index) {
        std::println("patch={} target={} tier={}",
                     liquid::to_string(composition->records[index]),
                     liquid::to_string(composition->patches[index].target.value),
                     liquid::to_string(composition->patches[index].tier));
    }
    return 0;
}

auto apply(int argc, char** argv) -> int {
    if (argc != 3 || std::string_view{argv[1]} != "--endpoint") {
        return usage("hot-update apply <patch> --endpoint <path>");
    }
    auto patch = liquid::parse_digest(argv[0]);
    if (!patch) {
        return fail(patch.error());
    }
    auto source = open_repository();
    if (!source) {
        return fail(source.error());
    }
    auto trust = signer_trust();
    if (!trust) {
        return fail(trust.error());
    }
    auto applied = liquid::runtime::block_on(liquid::hot_update::apply(
        *source, *patch, std::filesystem::path{argv[2]}, std::move(*trust)));
    if (!applied) {
        return fail(applied.error());
    }
    std::println("outcome={}", liquid::to_string(applied->outcome));
    std::println("patch={}", liquid::to_string(applied->record));
    std::println("target={}", liquid::to_string(applied->target.value));
    for (const auto& symbol : applied->symbols) {
        std::println("symbol={}", symbol);
    }
    return 0;
}

} // namespace

auto hot_update(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("hot-update requires classify, apply, or status");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "classify") {
        return classify(argc - 1, argv + 1);
    }
    if (command == "status") {
        return status(argc - 1, argv + 1);
    }
    if (command == "apply") {
        return apply(argc - 1, argv + 1);
    }
    return usage("hot-update requires classify, apply, or status");
}

} // namespace liquid_cli
