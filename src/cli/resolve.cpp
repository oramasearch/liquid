#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <optional>
#include <print>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/resolve.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto read_reuse(const std::filesystem::path& universe, liquid::resolve_options& options)
    -> liquid::result<void> {
    const auto path = universe / "reuse.txt";
    auto input = std::ifstream{path};
    if (!input) {
        return std::unexpected(liquid::error{liquid::errc::not_found,
                                             "--reuse requires reuse.txt in the universe directory",
                                             path.string()});
    }
    auto line = std::string{};
    while (std::getline(input, line)) {
        if (line.empty() || line.starts_with('#')) {
            continue;
        }
        auto parsed = liquid::parse_digest(line);
        if (!parsed) {
            return std::unexpected(liquid::error{
                liquid::errc::invalid_argument, "reuse.txt contains an invalid digest", line});
        }
        options.reuse.push_back(*parsed);
    }
    return {};
}

auto selected_spec(const liquid::concretised_spec& node,
                   const std::map<std::string, const liquid::concretised_spec*>& nodes)
    -> liquid::spec {
    auto output = liquid::spec{
        .name = node.name,
        .version = node.version,
        .variants = node.variants,
        .target = node.target,
    };
    if (node.compiler) {
        const auto compiler = nodes.find(liquid::to_string(*node.compiler));
        if (compiler != nodes.end()) {
            output.compiler = liquid::compiler_spec{
                .name = compiler->second->name,
                .version = compiler->second->version,
            };
        }
    }
    return output;
}

auto print_node(const liquid::concretised_spec& node,
                const std::map<std::string, const liquid::concretised_spec*>& nodes,
                std::set<std::string>& printed,
                std::size_t depth) -> void {
    std::println("{}{} {}",
                 std::string(depth * 2, ' '),
                 liquid::to_string(selected_spec(node, nodes)),
                 liquid::to_string(node.id));
    if (!printed.insert(liquid::to_string(node.id)).second) {
        return;
    }
    for (const auto& edge : node.dependencies) {
        const auto target = nodes.find(liquid::to_string(edge.target));
        if (target != nodes.end()) {
            print_node(*target->second, nodes, printed, depth + 1);
        }
    }
}

auto print_graph(const liquid::resolution_graph& graph) -> void {
    auto nodes = std::map<std::string, const liquid::concretised_spec*>{};
    for (const auto& node : graph.nodes) {
        nodes.emplace(liquid::to_string(node.id), &node);
    }
    auto printed = std::set<std::string>{};
    for (const auto& root : graph.roots) {
        const auto found = nodes.find(liquid::to_string(root));
        if (found != nodes.end()) {
            print_node(*found->second, nodes, printed, 0);
        }
    }
}

auto load_universe(const std::filesystem::path& path) -> liquid::result<liquid::package_universe> {
    if (!std::filesystem::is_directory(path / ".liquid-store")) {
        return liquid::runtime::block_on(liquid::load_package_universe(path));
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(path));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    return liquid::runtime::block_on(liquid::load_package_universe(*repository));
}

auto conflict_resolution(int argc, char** argv) -> int {
    auto conflict_id = std::optional<liquid::digest>{};
    auto request = liquid::conflict_resolution_request{};
    auto selected = false;
    for (auto index = 0; index != argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--accept" || argument == "--left" || argument == "--right") {
            if (selected) {
                return usage("resolve requires exactly one conflict resolution");
            }
            selected = true;
            request.kind = argument == "--accept"
                               ? liquid::conflict_resolution_kind::accept_proposal
                           : argument == "--left" ? liquid::conflict_resolution_kind::left
                                                  : liquid::conflict_resolution_kind::right;
            continue;
        }
        if (argument == "--from") {
            if (selected || ++index == argc) {
                return usage("resolve --from requires one file and no other resolution");
            }
            selected = true;
            request.kind = liquid::conflict_resolution_kind::blob;
            auto input = std::ifstream{argv[index], std::ios::binary};
            if (!input) {
                return usage("resolve --from cannot read the named file");
            }
            const auto bytes = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
            request.blob.reserve(bytes.size());
            for (const auto byte : bytes) {
                request.blob.push_back(std::byte{static_cast<unsigned char>(byte)});
            }
            continue;
        }
        if (argument.starts_with("--") || conflict_id) {
            return usage("resolve conflict arguments are invalid");
        }
        auto parsed = liquid::parse_digest(argument);
        if (!parsed) {
            return fail(parsed.error());
        }
        conflict_id = *parsed;
    }
    if (!conflict_id || !selected) {
        return usage("resolve <conflict-id> --accept|--left|--right|--from <file>");
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto resolved =
        liquid::runtime::block_on(repository->resolve_conflict(*conflict_id, std::move(request)));
    if (!resolved) {
        return fail(resolved.error());
    }
    std::println("change={} resolved={}",
                 liquid::to_string(resolved->value),
                 liquid::to_string(*conflict_id));
    return 0;
}

} // namespace

auto resolve(int argc, char** argv) -> int {
    const auto resolves_conflict = std::ranges::any_of(
        std::span{argv, static_cast<std::size_t>(argc)}, [](const auto* argument) {
            const auto value = std::string_view{argument};
            return value == "--accept" || value == "--left" || value == "--right" ||
                   value == "--from";
        });
    if (resolves_conflict) {
        return conflict_resolution(argc, argv);
    }
    auto universe_path = std::filesystem::current_path();
    auto roots = std::vector<liquid::spec>{};
    auto explain = false;
    auto json = false;
    auto reuse = false;
    auto why_package = std::string{};
    for (auto index = 0; index != argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--universe") {
            if (++index == argc) {
                return usage("resolve --universe requires a path");
            }
            universe_path = argv[index];
            continue;
        }
        if (argument == "--reuse") {
            reuse = true;
            continue;
        }
        if (argument == "--explain") {
            explain = true;
            continue;
        }
        if (argument == "--json") {
            json = true;
            continue;
        }
        if (argument == "--why") {
            if (++index == argc || !why_package.empty()) {
                return usage("resolve --why requires one package");
            }
            why_package = argv[index];
            continue;
        }
        if (argument.starts_with("--")) {
            return usage("resolve received an unknown option");
        }
        auto parsed = liquid::parse_spec(argument);
        if (!parsed) {
            return fail(parsed.error());
        }
        roots.push_back(std::move(*parsed));
    }
    if (roots.empty()) {
        return usage("resolve requires at least one specification");
    }
    auto universe = load_universe(universe_path);
    if (!universe) {
        return fail(universe.error());
    }
    auto options = liquid::resolve_options{};
    if (reuse) {
        auto loaded = read_reuse(universe_path, options);
        if (!loaded) {
            return fail(loaded.error());
        }
    }
    auto solved = liquid::runtime::block_on(
        liquid::resolve(std::move(*universe), std::move(roots), std::move(options)));
    if (!solved) {
        return fail(solved.error());
    }
    if (const auto* unsat = std::get_if<liquid::unsatisfiable>(&*solved)) {
        if (json) {
            std::println("{}", unsat->json);
        } else {
            std::println(stderr, "{}", unsat->narrative);
        }
        return 1;
    }
    const auto& graph = std::get<liquid::resolution_graph>(*solved);
    if (!why_package.empty()) {
        auto explanation = liquid::why(graph, why_package);
        if (!explanation) {
            return fail(explanation.error());
        }
        std::println("{}", json ? explanation->json : explanation->narrative);
        return 0;
    }
    if (json) {
        auto output = liquid::resolution_json(*solved);
        if (!output) {
            return fail(output.error());
        }
        std::println("{}", *output);
    } else {
        print_graph(graph);
    }
    if (explain && !json) {
        for (const auto& node : graph.nodes) {
            auto explanation = liquid::why(graph, node.name);
            if (!explanation) {
                return fail(explanation.error());
            }
            std::println("{}", explanation->narrative);
        }
    }
    return 0;
}

} // namespace liquid_cli
