#include "liquid/resolve.hpp"

#include <format>
#include <utility>

#include "resolve/concretizer.hpp"

namespace liquid {
namespace {

auto failure(errc code, std::string message, std::string context = "dependency resolution")
    -> error {
    return {code, std::move(message), std::move(context)};
}

auto escape_json(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
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
            output += character;
            break;
        }
    }
    return output;
}

auto type_name(dependency_type value) -> std::string_view {
    switch (value) {
    case dependency_type::build:
        return "build";
    case dependency_type::link:
        return "link";
    case dependency_type::run:
        return "run";
    }
    return "link";
}

} // namespace

runtime::task<resolution>
resolve(package_universe universe, std::vector<spec> roots, resolve_options options) {
    if (options.stop.stop_requested()) {
        co_return std::unexpected(failure(errc::busy, "resolution cancelled before grounding"));
    }
    const auto engine = resolve_detail::make_concretizer();
    auto grounded = co_await runtime::offload(
        [engine, universe = std::move(universe), roots = std::move(roots)]() mutable {
            return engine->ground(std::move(universe), std::move(roots));
        });
    if (!grounded) {
        co_return std::unexpected(grounded.error());
    }
    if (!*grounded) {
        co_return std::unexpected(grounded->error());
    }
    if (options.stop.stop_requested()) {
        co_return std::unexpected(failure(errc::busy, "resolution cancelled after grounding"));
    }
    auto solved = co_await runtime::offload(
        [engine, problem = std::move(**grounded), options = std::move(options)]() mutable {
            return engine->solve(std::move(problem), std::move(options));
        });
    if (!solved) {
        co_return std::unexpected(solved.error());
    }
    if (!*solved) {
        co_return std::unexpected(solved->error());
    }
    co_return std::move(**solved);
}

result<resolution_explanation> why(const resolution_graph& value, std::string_view package) {
    const auto found = std::ranges::find(value.nodes, package, &concretised_spec::name);
    if (found == value.nodes.end()) {
        return std::unexpected(failure(
            errc::not_found, "package is not present in the resolution", std::string{package}));
    }
    auto output = resolution_explanation{.package = found->name};
    output.narrative = found->name + "@" + found->version + " was selected.";
    for (const auto& reason : found->reasons) {
        output.narrative += "\n- ";
        output.narrative += reason;
    }
    output.json =
        std::format("{{\"version\":1,\"package\":\"{}\",\"reasons\":[", escape_json(found->name));
    for (std::size_t index = 0; index != found->reasons.size(); ++index) {
        if (index != 0) {
            output.json += ',';
        }
        output.json += '"' + escape_json(found->reasons[index]) + '"';
    }
    output.json += "],\"narrative\":\"" + escape_json(output.narrative) + "\"}";
    return output;
}

result<std::string> resolution_json(const resolution& value) {
    if (const auto* unsat = std::get_if<unsatisfiable>(&value)) {
        return unsat->json;
    }
    const auto& graph = std::get<resolution_graph>(value);
    auto output = std::format("{{\"version\":1,\"id\":\"{}\",\"roots\":[", to_string(graph.id));
    for (std::size_t index = 0; index != graph.roots.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += '"' + to_string(graph.roots[index]) + '"';
    }
    output += "],\"nodes\":[";
    for (std::size_t node_index = 0; node_index != graph.nodes.size(); ++node_index) {
        if (node_index != 0) {
            output += ',';
        }
        const auto& node = graph.nodes[node_index];
        output += std::format("{{\"id\":\"{}\",\"name\":\"{}\",\"package_version\":\"{}\","
                              "\"target\":\"{}\",\"variants\":{{",
                              to_string(node.id),
                              escape_json(node.name),
                              escape_json(node.version),
                              escape_json(node.target));
        auto variant_index = std::size_t{};
        for (const auto& [name, selected] : node.variants) {
            if (variant_index++ != 0) {
                output += ',';
            }
            output += '"' + escape_json(name) + "\":\"" + escape_json(selected) + '"';
        }
        output += "},\"compiler\":";
        output += node.compiler ? '"' + to_string(*node.compiler) + '"' : "null";
        output += ",\"dependencies\":[";
        for (std::size_t edge_index = 0; edge_index != node.dependencies.size(); ++edge_index) {
            if (edge_index != 0) {
                output += ',';
            }
            const auto& edge = node.dependencies[edge_index];
            output +=
                std::format("{{\"name\":\"{}\",\"type\":\"{}\",\"target\":\"{}\",\"virtual\":{}}}",
                            escape_json(edge.name),
                            type_name(edge.type),
                            to_string(edge.target),
                            edge.virtual_dependency ? "true" : "false");
        }
        output += "],\"reasons\":[";
        for (std::size_t reason_index = 0; reason_index != node.reasons.size(); ++reason_index) {
            if (reason_index != 0) {
                output += ',';
            }
            output += '"' + escape_json(node.reasons[reason_index]) + '"';
        }
        output += "]}";
    }
    output += "]}";
    return output;
}

} // namespace liquid
