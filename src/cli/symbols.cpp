#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

auto frontend_module_path(bool compiler) -> std::filesystem::path;

namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr, "error: usage: symbols requires one path");
    return 2;
}

auto kind_name(liquid::symbol_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::symbol_kind::function:
        return "function";
    case liquid::symbol_kind::type:
        return "type";
    case liquid::symbol_kind::variable:
        return "variable";
    case liquid::symbol_kind::field:
        return "field";
    case liquid::symbol_kind::parameter:
        return "parameter";
    case liquid::symbol_kind::namespace_:
        return "namespace";
    default:
        return "unknown";
    }
}

} // namespace

auto symbols(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage();
    }
    const auto path = std::string{argv[0]};
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto source = liquid::runtime::block_on(repository->read(path));
    if (!source) {
        return fail(source.error());
    }
    auto subject = liquid::hash(liquid::algorithm::blake3, *source);
    if (!subject) {
        return fail(subject.error());
    }
    auto compiler = liquid::frontend_module::load(frontend_module_path(true));
    auto raw = compiler ? compiler->symbols("c++", *source)
                        : liquid::result<std::vector<std::byte>>{std::unexpected(compiler.error())};
    if (!raw) {
        auto fallback = liquid::frontend_module::load(frontend_module_path(false));
        if (!fallback) {
            return fail(fallback.error());
        }
        raw = fallback->symbols("c++", *source);
    }
    if (!raw) {
        return fail(raw.error());
    }
    auto record = liquid::decode_frontend_symbols(*subject, *raw);
    if (!record) {
        return fail(record.error());
    }
    auto stored = liquid::runtime::block_on(repository->store_symbols(*record));
    if (!stored) {
        return fail(stored.error());
    }
    std::println("origin={} record={}",
                 record->origin == liquid::symbol_origin::compiler ? "compiler" : "syntactic",
                 liquid::to_string(*stored));
    for (const auto& occurrence : record->occurrences) {
        std::println("{} {} {} {}:{}:{}",
                     occurrence.role == liquid::symbol_role::definition ? "definition"
                                                                        : "reference",
                     kind_name(occurrence.kind),
                     occurrence.symbol,
                     occurrence.range.start.line,
                     occurrence.range.start.column,
                     occurrence.name);
    }
    return 0;
}

} // namespace liquid_cli
