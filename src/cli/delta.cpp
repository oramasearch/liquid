#include <filesystem>
#include <print>
#include <string_view>
#include <utility>

#include "liquid/delta.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"

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

auto open_store(std::filesystem::path path) -> liquid::result<liquid::store> {
    return liquid::runtime::block_on(liquid::store::open(std::move(path)));
}

auto create(int argc, char** argv) -> int {
    if (argc != 2) {
        return usage("delta create <source> <target>");
    }
    auto store = open_store(std::filesystem::current_path() / ".liquid-store");
    if (!store) {
        return fail(store.error());
    }
    auto created = liquid::runtime::block_on(liquid::create_delta(
        *store, std::filesystem::path{argv[0]}, std::filesystem::path{argv[1]}));
    if (!created) {
        return fail(created.error());
    }
    std::println("source={}", liquid::to_string(created->value.source));
    std::println("target={}", liquid::to_string(created->value.target));
    std::println("algorithm={}", liquid::to_string(created->value.algorithm));
    std::println("delta={}", liquid::to_string(created->record));
    std::println("attestation={}", liquid::to_string(created->attestation));
    std::println("source_bytes={}", created->source_bytes);
    std::println("target_bytes={}", created->target_bytes);
    std::println("delta_bytes={}", created->delta_bytes);
    return 0;
}

auto apply(int argc, char** argv) -> int {
    if (argc != 3) {
        return usage("delta apply <delta> <source> <destination>");
    }
    auto id = liquid::parse_digest(argv[0]);
    if (!id) {
        return fail(id.error());
    }
    auto store = open_store(std::filesystem::current_path() / ".liquid-store");
    if (!store) {
        return fail(store.error());
    }
    auto applied = liquid::runtime::block_on(liquid::apply_delta(
        *store, *id, std::filesystem::path{argv[1]}, std::filesystem::path{argv[2]}));
    if (!applied) {
        return fail(applied.error());
    }
    std::println("applied={}", liquid::to_string(*id));
    return 0;
}

auto fetch(int argc, char** argv) -> int {
    if (argc != 3) {
        return usage("delta fetch <target> <source> <destination>");
    }
    auto target = liquid::parse_digest(argv[0]);
    if (!target) {
        return fail(target.error());
    }
    auto local = open_store(std::filesystem::current_path() / ".liquid-store");
    if (!local) {
        return fail(local.error());
    }
    auto fetched =
        liquid::runtime::block_on(liquid::fetch_delta(*local, *target, argv[1], argv[2]));
    if (!fetched) {
        return fail(fetched.error());
    }
    std::println("method={}", liquid::to_string(fetched->method));
    std::println("target={}", liquid::to_string(fetched->target));
    if (fetched->record) {
        std::println("delta={}", liquid::to_string(*fetched->record));
    }
    if (fetched->attestation) {
        std::println("attestation={}", liquid::to_string(*fetched->attestation));
    }
    return 0;
}

} // namespace

auto delta(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("delta requires create, apply, or fetch");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "create") {
        return create(argc - 1, argv + 1);
    }
    if (command == "apply") {
        return apply(argc - 1, argv + 1);
    }
    if (command == "fetch") {
        return fetch(argc - 1, argv + 1);
    }
    return usage("delta requires create, apply, or fetch");
}

} // namespace liquid_cli
