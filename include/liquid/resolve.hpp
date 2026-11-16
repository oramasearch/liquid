// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <stop_token>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

enum class dependency_type : std::uint8_t {
    build,
    link,
    run,
};

struct LIQUID_EXPORT compiler_spec {
    std::string name;
    std::optional<std::string> version;

    auto operator==(const compiler_spec&) const -> bool = default;
};

struct LIQUID_EXPORT spec {
    std::string name;
    std::optional<std::string> version;
    std::map<std::string, std::string> variants;
    std::optional<compiler_spec> compiler;
    std::optional<std::string> target;
    std::vector<spec> dependencies;

    auto operator==(const spec&) const -> bool = default;
};

struct LIQUID_EXPORT package_version {
    std::string version;
    std::optional<digest> source;
};

struct LIQUID_EXPORT variant_definition {
    std::string name;
    std::vector<std::string> values;
    std::string default_value;
    std::optional<std::string> when;
};

struct LIQUID_EXPORT dependency_definition {
    std::string name;
    bool virtual_dependency{};
    std::optional<std::string> version;
    std::optional<std::string> when;
    dependency_type type{dependency_type::link};
};

struct LIQUID_EXPORT provide_definition {
    std::string name;
    std::optional<std::string> version;
};

struct LIQUID_EXPORT conflict_definition {
    spec constraint;
    std::optional<std::string> when;
    std::string message;
};

struct LIQUID_EXPORT package_patch {
    digest value{};
    std::optional<std::string> when;
};

struct LIQUID_EXPORT package_record {
    std::string name;
    std::vector<package_version> versions;
    std::vector<variant_definition> variants;
    std::vector<dependency_definition> dependencies;
    std::vector<provide_definition> provides;
    std::vector<conflict_definition> conflicts;
    std::vector<std::string> compiler_constraints;
    std::vector<std::string> target_constraints;
    std::vector<package_patch> patches;
};

struct LIQUID_EXPORT package_universe {
    std::vector<package_record> packages;
};

struct LIQUID_EXPORT concretised_edge {
    std::string name;
    dependency_type type{dependency_type::link};
    digest target{};
    bool virtual_dependency{};
};

struct LIQUID_EXPORT concretised_spec {
    digest id{};
    std::string name;
    std::string version;
    std::map<std::string, std::string> variants;
    std::optional<digest> compiler;
    std::string target;
    std::vector<concretised_edge> dependencies;
    std::vector<digest> patches;
    std::vector<std::string> reasons;
};

enum class constraint_origin_kind : std::uint8_t {
    root,
    dependency,
    variant_default,
    conflict,
    compiler,
    target,
};

struct LIQUID_EXPORT constraint_origin {
    constraint_origin_kind kind{constraint_origin_kind::root};
    std::string package;
    std::string detail;
};

struct LIQUID_EXPORT unsat_constraint {
    std::string constraint;
    constraint_origin origin;
};

struct LIQUID_EXPORT unsatisfiable {
    std::vector<unsat_constraint> core;
    std::string narrative;
    std::string json;
};

struct LIQUID_EXPORT resolution_graph {
    digest id{};
    std::vector<digest> roots;
    std::vector<concretised_spec> nodes;
};

using resolution = std::variant<resolution_graph, unsatisfiable>;

struct LIQUID_EXPORT compatibility_fact {
    digest built{};
    digest candidate{};
    bool compatible{};
};

struct LIQUID_EXPORT resolve_options {
    std::vector<digest> reuse;
    std::vector<digest> known_outputs;
    std::vector<compatibility_fact> compatibility;
    std::stop_token stop;
};

struct LIQUID_EXPORT resolution_explanation {
    std::string package;
    std::string narrative;
    std::string json;
};

[[nodiscard]] LIQUID_EXPORT result<spec> parse_spec(std::string_view text);
[[nodiscard]] LIQUID_EXPORT std::string to_string(const spec& value);

[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_package(const package_record& value);
[[nodiscard]] LIQUID_EXPORT result<package_record> decode_package(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_concretised_spec(const concretised_spec& value);
[[nodiscard]] LIQUID_EXPORT result<concretised_spec>
decode_concretised_spec(std::span<const std::byte> bytes);

[[nodiscard]] LIQUID_EXPORT runtime::task<package_universe>
load_package_universe(std::filesystem::path directory);
[[nodiscard]] LIQUID_EXPORT runtime::task<package_universe>
load_package_universe(repository& source);

[[nodiscard]] LIQUID_EXPORT runtime::task<resolution>
resolve(package_universe universe, std::vector<spec> roots, resolve_options options = {});
[[nodiscard]] LIQUID_EXPORT result<resolution_explanation> why(const resolution_graph& value,
                                                               std::string_view package);
[[nodiscard]] LIQUID_EXPORT result<std::string> resolution_json(const resolution& value);

} // namespace liquid
