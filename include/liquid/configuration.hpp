// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/install.hpp"
#include "liquid/resolve.hpp"
#include "liquid/runtime.hpp"
#include "liquid/session.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT configuration_location {
    std::filesystem::path file;
    std::uint64_t line{1};
    std::uint64_t column{1};

    auto operator==(const configuration_location&) const -> bool = default;
};

enum class configuration_value_kind : std::uint8_t {
    string,
    integer,
    boolean,
    version,
    spec,
    digest,
    path,
    identifier,
    list,
    map,
};

struct LIQUID_EXPORT configuration_value {
    configuration_value_kind kind{configuration_value_kind::identifier};
    configuration_location location;
    std::string text;
    std::int64_t integer{};
    bool boolean{};
    std::vector<configuration_value> list;
    std::map<std::string, configuration_value> map;

    auto operator==(const configuration_value&) const -> bool = default;
};

struct LIQUID_EXPORT configuration_field {
    std::string name;
    configuration_location location;
    configuration_value value;

    auto operator==(const configuration_field&) const -> bool = default;
};

struct LIQUID_EXPORT configuration_record {
    std::string kind;
    std::optional<std::string> name;
    configuration_location location;
    std::vector<configuration_field> fields;
    std::vector<configuration_record> records;
    std::optional<spec> condition;
    std::optional<std::string> condition_text;

    auto operator==(const configuration_record&) const -> bool = default;
};

struct LIQUID_EXPORT configuration_document {
    std::filesystem::path origin;
    std::vector<configuration_record> records;

    auto operator==(const configuration_document&) const -> bool = default;
};

struct LIQUID_EXPORT configuration_diagnostic {
    configuration_location location;
    std::string expected;
    std::string found;
    std::string hint;

    auto operator==(const configuration_diagnostic&) const -> bool = default;
};

enum class configuration_origin_kind : std::uint8_t {
    source,
    generated,
};

struct LIQUID_EXPORT configuration_record_origin {
    configuration_origin_kind kind{configuration_origin_kind::source};
    std::filesystem::path file;
    std::optional<digest> result;
};

enum class source_kind : std::uint8_t {
    archive,
    file,
};

struct LIQUID_EXPORT source {
    std::uint64_t v{1};
    std::string name;
    std::string url;
    digest content{};
    source_kind kind{source_kind::archive};
    configuration_record_origin origin;
};

struct LIQUID_EXPORT target_dependency {
    std::string name;
    std::optional<std::string> when;

    auto operator==(const target_dependency&) const -> bool = default;
};

struct LIQUID_EXPORT target {
    std::uint64_t v{1};
    std::string name;
    std::vector<std::string> command;
    std::string cwd{"."};
    std::map<std::string, std::string> environment;
    std::vector<digest> sources;
    std::vector<target_dependency> dependencies;
    std::vector<std::string> outputs;
    std::string install_layout{"prefix"};
    std::optional<install_intent> install;
    std::optional<digest> inputs;
    std::optional<digest> toolchain;
    configuration_record_origin origin;
};

struct LIQUID_EXPORT configuration_generator {
    std::vector<std::string> command;
    std::string cwd{"."};
    std::map<std::string, std::string> environment;
    std::string output;
    std::string format{"liquid"};
    digest inputs{};
    digest toolchain{};
    std::vector<std::string> declared_outputs;
};

struct LIQUID_EXPORT lowered_configuration {
    std::vector<package_record> packages;
    std::vector<configuration_record_origin> package_origins;
    std::vector<target> targets;
    std::vector<source> sources;
    std::vector<configuration_generator> generators;
    lock_config locks;
};

[[nodiscard]] LIQUID_EXPORT result<configuration_document>
parse_configuration(std::string_view text, std::filesystem::path origin);
[[nodiscard]] LIQUID_EXPORT std::vector<configuration_diagnostic>
check(const configuration_document& document);
[[nodiscard]] LIQUID_EXPORT result<lowered_configuration>
lower(const configuration_document& document);
[[nodiscard]] LIQUID_EXPORT std::string to_string(const configuration_diagnostic& diagnostic);
[[nodiscard]] LIQUID_EXPORT result<std::string>
format_configuration(const configuration_document& document);

[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>> encode_target(const target& value);
[[nodiscard]] LIQUID_EXPORT result<target> decode_target(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>> encode_source(const source& value);
[[nodiscard]] LIQUID_EXPORT result<source> decode_source(std::span<const std::byte> bytes);

[[nodiscard]] LIQUID_EXPORT runtime::task<lowered_configuration>
run_generators(repository& repository, configuration_document document);

[[nodiscard]] LIQUID_EXPORT runtime::task<result_id> execute_target(repository& repository,
                                                                    const target& target);
[[nodiscard]] LIQUID_EXPORT runtime::task<result_id> build_configuration(
    repository& repository, const lowered_configuration& configuration, std::string target_name);

} // namespace liquid
