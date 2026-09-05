#include <filesystem>
#include <memory>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
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

auto input_tree_contains_path(liquid::repository& repository,
                              liquid::digest root,
                              std::string_view path) -> liquid::result<bool> {
    auto parts = std::vector<std::string>{};
    for (const auto& part : std::filesystem::path{path}) {
        const auto value = part.generic_string();
        if (!value.empty() && value != ".") {
            parts.push_back(value);
        }
    }
    if (parts.empty()) {
        return false;
    }

    for (auto index = std::size_t{}; index != parts.size(); ++index) {
        auto tree = liquid::runtime::block_on(repository.tree(root));
        if (!tree) {
            return std::unexpected(liquid::error{
                tree.error().code, tree.error().message, "input tree " + liquid::to_string(root)});
        }
        const auto found =
            std::ranges::find(tree->entries, parts[index], &liquid::tree_entry::name);
        if (found == tree->entries.end()) {
            return false;
        }
        if (index + 1 == parts.size()) {
            return true;
        }
        if (found->kind != liquid::node_kind::tree) {
            return false;
        }
        root = found->ref;
    }
    return false;
}

auto is_compile(const liquid::action_input& input) -> bool {
    return std::ranges::find(input.command, "-c") != input.command.end();
}

auto is_translation_unit(std::string_view path) -> bool {
    const auto extension = std::filesystem::path{path}.extension();
    return extension == ".c" || extension == ".cc" || extension == ".cpp" || extension == ".cxx";
}

auto translation_unit_path(const liquid::action_result& result) -> std::optional<std::string> {
    const auto source = std::ranges::find_if(
        result.manifest, [](const auto& input) { return is_translation_unit(input.path); });
    return source == result.manifest.end() ? std::nullopt
                                           : std::optional<std::string>{source->path};
}

struct recorded_compile {
    liquid::action_input action;
    std::string id;
    std::string source_path;
};

auto recorded_compile_actions(liquid::repository& repository, std::string_view path)
    -> liquid::result<std::vector<recorded_compile>> {
    auto filter = liquid::result_filter{};
    auto actions = std::vector<recorded_compile>{};
    for (;;) {
        auto page = liquid::runtime::block_on(repository.results(filter));
        if (!page) {
            return std::unexpected(page.error());
        }
        for (const auto& result : page->records) {
            auto action = liquid::runtime::block_on(repository.action(result.value.action));
            if (!action) {
                return std::unexpected(
                    liquid::error{action.error().code,
                                  action.error().message,
                                  "recorded action " + liquid::to_string(result.value.action)});
            }
            auto contains_path = input_tree_contains_path(repository, action->inputs, path);
            if (!contains_path) {
                return std::unexpected(contains_path.error());
            }
            if (!*contains_path) {
                continue;
            }
            auto source_path = translation_unit_path(result.value);
            if (is_compile(*action) && source_path) {
                actions.push_back(
                    {std::move(*action), liquid::to_string(result.value.action), *source_path});
            }
        }
        if (page->done) {
            break;
        }
        filter.page.cursor = page->next;
    }
    if (!actions.empty()) {
        return actions;
    }
    return std::unexpected(liquid::error{liquid::errc::not_found,
                                         "no recorded compile action for " + std::string{path} +
                                             "; run `liquid build` first",
                                         std::string{path}});
}

auto syntactic_symbols(std::span<const std::byte> source, liquid::digest subject)
    -> liquid::result<liquid::symbol_record> {
    auto fallback = liquid::frontend_module::load(frontend_module_path(false));
    if (!fallback) {
        return std::unexpected(fallback.error());
    }
    auto raw = fallback->symbols("c++", source);
    if (!raw) {
        return std::unexpected(raw.error());
    }
    return liquid::decode_frontend_symbols(subject, *raw);
}

} // namespace

auto symbols(int argc, char** argv) -> int {
    if (argc != 1) {
        return usage();
    }
    const auto path = std::string{argv[0]};
    auto repository_path = std::filesystem::current_path();
    auto cache_directory = liquid::build::wrap_cache_directory(repository_path);
    if (!cache_directory) {
        return fail(cache_directory.error());
    }
    const auto build_repository = *cache_directory / "repository";
    if (std::filesystem::exists(build_repository / "LIQUID_REPO")) {
        repository_path = build_repository;
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(repository_path));
    if (!repository) {
        return fail(repository.error());
    }
    auto compiler = liquid::frontend_module::load(frontend_module_path(true));
    auto records = std::vector<liquid::symbol_record>{};
    if (!compiler) {
        auto source = liquid::runtime::block_on(repository->read(path));
        if (!source) {
            return fail(source.error());
        }
        auto subject = liquid::hash(liquid::algorithm::blake3, *source);
        if (!subject) {
            return fail(subject.error());
        }
        auto record = syntactic_symbols(*source, *subject);
        if (!record) {
            return fail(record.error());
        }
        records.push_back(std::move(*record));
    } else {
        auto recorded = recorded_compile_actions(*repository, path);
        if (!recorded) {
            return fail(recorded.error());
        }
        for (auto& compile : *recorded) {
            auto source = liquid::runtime::block_on(repository->read(compile.source_path));
            if (!source) {
                return fail(liquid::error{source.error().code,
                                          source.error().message,
                                          "recorded source " + compile.source_path});
            }
            auto action = std::make_shared<liquid::action_input>(std::move(compile.action));
            auto module_value = liquid::frontend_module::load(frontend_module_path(true));
            if (!module_value) {
                return fail(module_value.error());
            }
            auto module = std::make_shared<liquid::frontend_module>(std::move(*module_value));
            auto graph = liquid::runtime::block_on(liquid::frontend_graph::open(
                std::filesystem::current_path() / ".liquid-build" / "symbols-graph" / compile.id,
                std::filesystem::current_path(),
                module,
                [&repository, action](std::vector<std::byte> descriptor) {
                    return liquid::build::execute_symbol_action(
                        *repository, *action, std::move(descriptor));
                }));
            if (!graph) {
                return fail(liquid::error{
                    graph.error().code, graph.error().message, "symbol graph " + compile.id});
            }
            auto record =
                liquid::runtime::block_on(graph->symbols({.language = "c++",
                                                          .source_path = compile.source_path,
                                                          .source = *source,
                                                          .compile_action = compile.id}));
            if (!record) {
                return fail(liquid::error{
                    record.error().code, record.error().message, "symbol query " + compile.id});
            }
            records.push_back(std::move(*record));
        }
    }
    for (const auto& record : records) {
        auto stored = liquid::runtime::block_on(repository->store_symbols(record));
        if (!stored) {
            return fail(stored.error());
        }
        std::println("origin={} record={}",
                     record.origin == liquid::symbol_origin::compiler ? "compiler" : "syntactic",
                     liquid::to_string(*stored));
        for (const auto& occurrence : record.occurrences) {
            std::println("{} {} {} {}:{}:{}",
                         occurrence.role == liquid::symbol_role::definition ? "definition"
                                                                            : "reference",
                         kind_name(occurrence.kind),
                         occurrence.symbol,
                         occurrence.range.start.line,
                         occurrence.range.start.column,
                         occurrence.name);
        }
    }
    return 0;
}

} // namespace liquid_cli
