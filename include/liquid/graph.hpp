// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

enum class durability : std::uint8_t { volatile_ = 0, low = 1, medium = 2, high = 3 };

enum class graph_input_kind : std::uint8_t {
    working_tree_blob = 1,
    overlay_buffer = 2,
    environment = 3,
    toolchain = 4
};

struct LIQUID_EXPORT graph_input_key {
    graph_input_kind kind{graph_input_kind::working_tree_blob};
    std::string name;

    auto operator==(const graph_input_key&) const -> bool = default;
};

struct LIQUID_EXPORT query_key {
    std::string kind;
    std::vector<std::byte> arguments;

    auto operator==(const query_key&) const -> bool = default;
};

using graph_dependency_key = std::variant<query_key, graph_input_key>;

struct LIQUID_EXPORT graph_value {
    std::vector<std::byte> bytes;
    digest fingerprint;
    durability durability_tier{durability::high};
};

struct LIQUID_EXPORT recorded_dependency {
    graph_dependency_key key;
    digest fingerprint;
};

struct LIQUID_EXPORT recorded_query {
    query_key key;
    digest fingerprint;
    std::vector<recorded_dependency> dependencies;
    graph_value value;
};

struct LIQUID_EXPORT graph_statistics {
    std::uint64_t demands{};
    std::uint64_t hits{};
    std::uint64_t executions{};
    std::uint64_t dependency_reads{};
    std::uint64_t fingerprint_reads{};
    std::uint64_t invalidation_walks{};
    std::uint64_t discarded_computations{};
    std::uint64_t keys{};
    std::uint64_t resident_values{};
    std::uint64_t resident_value_bytes{};
    std::uint64_t memory_ceiling{};
};

struct LIQUID_EXPORT graph_validation_record {
    digest id;
    query_key query;
    std::vector<recorded_dependency> inputs;
    digest incremental;
    digest clean;
    bool agreement{};
    std::chrono::sys_seconds at;
};

struct LIQUID_EXPORT graph_validation_options {
    std::uint32_t rate{20};
    bool all{};
    std::uint64_t limit{};
};

struct LIQUID_EXPORT graph_validation_status {
    std::uint64_t eligible{};
    std::uint64_t validated{};
    std::uint64_t backlog{};
    std::uint64_t records{};
    std::optional<graph_validation_record> last_divergence;
};

struct LIQUID_EXPORT graph_validation_report {
    std::uint64_t considered{};
    std::uint64_t validated{};
    std::uint64_t divergences{};
    std::uint64_t backlog{};
    std::chrono::milliseconds elapsed{};
    std::vector<graph_validation_record> records;
    std::optional<graph_validation_record> last_divergence;
};

class query_context;

using query_argument_encoder =
    std::function<result<std::vector<std::byte>>(std::span<const std::byte>)>;
using query_compute =
    std::function<runtime::task<std::vector<std::byte>>(query_context&, std::vector<std::byte>)>;

struct LIQUID_EXPORT query_kind {
    std::string name;
    query_argument_encoder encode_arguments;
    query_compute compute;
};

class LIQUID_EXPORT query_registry {
public:
    query_registry();
    ~query_registry();
    query_registry(const query_registry&) noexcept;
    query_registry& operator=(const query_registry&) noexcept;
    query_registry(query_registry&&) noexcept;
    query_registry& operator=(query_registry&&) noexcept;

    [[nodiscard]] result<void> add(query_kind kind);
    [[nodiscard]] result<query_key> key(std::string kind,
                                        std::span<const std::byte> arguments) const;

private:
    friend class graph;
    friend class query_context;
    struct impl;
    std::shared_ptr<impl> impl_;
};

struct LIQUID_EXPORT graph_options {
    std::filesystem::path working_tree;
    std::uint64_t memory_ceiling{};
};

class LIQUID_EXPORT graph {
public:
    graph() noexcept;
    ~graph();
    graph(graph&&) noexcept;
    graph& operator=(graph&&) noexcept;
    graph(const graph&) = delete;
    graph& operator=(const graph&) = delete;

    [[nodiscard]] static runtime::task<graph>
    open(std::filesystem::path directory, query_registry registry, graph_options options = {});
    [[nodiscard]] runtime::task<graph_value> demand(query_key key);
    [[nodiscard]] runtime::task<void>
    set_input(graph_input_key key, std::vector<std::byte> value, durability tier);
    [[nodiscard]] runtime::task<void> clear_input(graph_input_key key, durability tier);
    [[nodiscard]] result<std::uint64_t> mark_changed(graph_input_key key, durability tier);
    [[nodiscard]] result<std::uint64_t> mark_changed(query_key key);
    [[nodiscard]] runtime::task<std::optional<recorded_query>> recorded(query_key key);
    [[nodiscard]] runtime::task<graph_validation_report>
    validate(graph_validation_options options = {});
    [[nodiscard]] graph_validation_status validation_status() const;
    [[nodiscard]] runtime::task<std::vector<graph_validation_record>> validation_records();
    [[nodiscard]] runtime::task<void> inject_stale_value_for_testing(query_key key,
                                                                     std::vector<std::byte> value);
    [[nodiscard]] runtime::task<void> flush();
    [[nodiscard]] graph_statistics statistics() const;

private:
    friend class query_context;
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT query_context {
public:
    query_context(query_context&&) noexcept;
    query_context& operator=(query_context&&) noexcept;
    query_context(const query_context&) = delete;
    query_context& operator=(const query_context&) = delete;
    ~query_context();

    [[nodiscard]] runtime::task<graph_value> read(graph_dependency_key key);

private:
    friend struct graph::impl;
    struct impl;
    explicit query_context(std::unique_ptr<impl> implementation);
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
