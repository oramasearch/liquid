#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/frontend_abi.h"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

enum class frontend_diagnostic_severity : std::uint8_t {
    note,
    warning,
    error,
    fatal,
};

struct LIQUID_EXPORT frontend_diagnostic {
    std::string file;
    std::uint64_t line{};
    std::uint64_t column{};
    frontend_diagnostic_severity severity{frontend_diagnostic_severity::error};
    std::string message;
};

enum class frontend_unit_kind : std::uint8_t {
    declaration = 1,
    definition = 2,
    type = 3,
};

struct LIQUID_EXPORT frontend_source_position {
    std::uint64_t byte{};
    std::uint64_t line{};
    std::uint64_t column{};
};

struct LIQUID_EXPORT frontend_source_range {
    frontend_source_position start;
    frontend_source_position end;
};

struct LIQUID_EXPORT frontend_unit {
    frontend_unit_kind kind{frontend_unit_kind::declaration};
    std::string name;
    frontend_source_range range;
    digest fingerprint;
    std::vector<std::byte> canonical_value;
};

struct LIQUID_EXPORT syntax_node {
    std::string kind;
    bool named{};
    std::optional<std::string> field;
    frontend_source_range range;
    std::vector<syntax_node> children;

    syntax_node() = default;
    syntax_node(const syntax_node& other);
    syntax_node(syntax_node&& other) noexcept;
    auto operator=(const syntax_node& other) -> syntax_node&;
    auto operator=(syntax_node&& other) noexcept -> syntax_node&;

    [[nodiscard]] auto node_at(std::uint64_t byte) const -> const syntax_node*;
    [[nodiscard]] auto enclosing(std::string_view node_kind) const -> const syntax_node*;

private:
    syntax_node* parent_{};

    void relink(syntax_node* parent = nullptr);
    friend auto decode_syntax_tree(std::span<const std::byte> value) -> result<struct syntax_tree>;
};

struct LIQUID_EXPORT syntax_tree {
    std::string language;
    syntax_node root;
};

struct LIQUID_EXPORT parse_node {
    std::uint32_t kind{};
    std::optional<std::string> field;
    frontend_source_range range;
    bool error{};
    bool missing{};
    std::vector<parse_node> children;
};

struct LIQUID_EXPORT parse_tree {
    std::string language;
    std::string grammar;
    std::string node_types;
    std::vector<std::string> node_kinds;
    parse_node root;
    std::vector<frontend_unit> units;
};

[[nodiscard]] LIQUID_EXPORT auto decode_syntax_tree(std::span<const std::byte> value)
    -> result<syntax_tree>;
[[nodiscard]] LIQUID_EXPORT auto decode_parse_tree(std::span<const std::byte> value)
    -> result<parse_tree>;

struct LIQUID_EXPORT frontend_callbacks {
    std::function<void(std::string_view, std::uint64_t, std::uint64_t)> progress;
    std::function<bool()> cancelled;
};

enum class symbol_origin : std::uint8_t { compiler = 1, syntactic = 2 };
enum class symbol_role : std::uint8_t { definition = 1, reference = 2 };
enum class symbol_kind : std::uint8_t {
    unknown = 0,
    function = 1,
    type = 2,
    variable = 3,
    field = 4,
    parameter = 5,
    namespace_ = 6,
};

struct LIQUID_EXPORT symbol_occurrence {
    std::string symbol;
    std::string name;
    symbol_kind kind{symbol_kind::unknown};
    symbol_role role{symbol_role::reference};
    frontend_source_range range;
    std::optional<std::string> container;
    std::optional<std::string> documentation;
};

struct LIQUID_EXPORT symbol_record {
    digest subject;
    std::string language;
    symbol_origin origin{symbol_origin::syntactic};
    std::string grammar;
    std::vector<symbol_occurrence> occurrences;
};

enum class frontend_tier_mechanism : std::uint8_t { frontend = 1, grammar = 2 };

struct LIQUID_EXPORT frontend_language {
    std::string language;
    std::vector<std::string> extensions;
    frontend_tier_mechanism tier_mechanism{frontend_tier_mechanism::frontend};
    std::string grammar_digest;
};

[[nodiscard]] LIQUID_EXPORT auto encode_symbols(const symbol_record& value)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto decode_symbols(std::span<const std::byte> value)
    -> result<symbol_record>;
[[nodiscard]] LIQUID_EXPORT auto decode_frontend_symbols(digest subject,
                                                         std::span<const std::byte> value)
    -> result<symbol_record>;

class LIQUID_EXPORT frontend_module {
public:
    frontend_module() noexcept;
    ~frontend_module();
    frontend_module(frontend_module&&) noexcept;
    auto operator=(frontend_module&&) noexcept -> frontend_module&;
    frontend_module(const frontend_module&) = delete;
    auto operator=(const frontend_module&) -> frontend_module& = delete;

    static auto load(std::filesystem::path shared_object) -> result<frontend_module>;
    static auto linked(const liquid_frontend_module_v1* descriptor) -> result<frontend_module>;

    [[nodiscard]] auto name() const -> std::string_view;
    [[nodiscard]] auto languages() const -> std::vector<std::string>;
    [[nodiscard]] auto language_details() const -> std::vector<frontend_language>;
    [[nodiscard]] auto supports(std::string_view language) const -> bool;
    [[nodiscard]] auto units(std::string_view language,
                             std::span<const std::byte> blob,
                             std::span<const std::byte> context_cbor = {},
                             frontend_callbacks callbacks = {}) const
        -> result<std::vector<frontend_unit>>;
    [[nodiscard]] auto compile_unit(const frontend_unit& unit,
                                    std::span<const std::byte> context_cbor = {},
                                    frontend_callbacks callbacks = {}) const
        -> result<std::vector<std::byte>>;
    [[nodiscard]] auto assemble(std::span<const std::byte> unit_outputs_cbor,
                                std::span<const std::byte> context_cbor = {},
                                frontend_callbacks callbacks = {}) const
        -> result<std::vector<std::byte>>;
    [[nodiscard]] auto symbols(std::string_view language,
                               std::span<const std::byte> blob,
                               std::span<const std::byte> context_cbor = {},
                               frontend_callbacks callbacks = {}) const
        -> result<std::vector<std::byte>>;
    [[nodiscard]] auto symbols_from_output(std::string_view language,
                                           std::span<const std::byte> blob,
                                           std::span<const std::byte> action_output,
                                           frontend_callbacks callbacks = {}) const
        -> result<std::vector<std::byte>>;
    [[nodiscard]] auto syntax(std::string_view language,
                              std::span<const std::byte> blob,
                              std::span<const std::byte> context_cbor = {},
                              frontend_callbacks callbacks = {}) const -> result<syntax_tree>;
    [[nodiscard]] auto parse(std::string_view language,
                             std::span<const std::byte> blob,
                             std::span<const std::byte> context_cbor = {},
                             frontend_callbacks callbacks = {}) const
        -> result<std::vector<std::byte>>;

private:
    struct impl;
    explicit frontend_module(std::unique_ptr<impl> implementation);
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT frontend_registry {
public:
    frontend_registry();
    ~frontend_registry();
    frontend_registry(frontend_registry&&) noexcept;
    auto operator=(frontend_registry&&) noexcept -> frontend_registry&;
    frontend_registry(const frontend_registry&) = delete;
    auto operator=(const frontend_registry&) -> frontend_registry& = delete;

    auto add(frontend_module module) -> result<void>;
    auto load(std::filesystem::path shared_object) -> result<void>;
    [[nodiscard]] auto find(std::string_view language) const -> result<const frontend_module*>;
    [[nodiscard]] auto languages() const -> std::vector<frontend_language>;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

struct LIQUID_EXPORT frontend_build_request {
    std::string language;
    std::string source_path;
    std::vector<std::byte> source;
    std::vector<std::byte> context_cbor;
    std::string compile_action;
};

struct LIQUID_EXPORT frontend_parse_request {
    std::string language;
    digest blob;
    std::vector<std::byte> source;
};

struct LIQUID_EXPORT frontend_unit_output {
    frontend_unit unit;
    std::vector<std::byte> action;
    std::vector<std::byte> output;
    bool action_ran{};
};

struct LIQUID_EXPORT frontend_build_report {
    std::vector<frontend_unit_output> units;
    std::vector<std::byte> translation_unit_output;
    symbol_record symbols;
    std::uint64_t actions_run{};
    std::uint64_t assemblies_run{};
};

using frontend_action_executor =
    std::function<runtime::task<std::vector<std::byte>>(std::vector<std::byte>)>;

class LIQUID_EXPORT frontend_graph {
public:
    frontend_graph() noexcept;
    ~frontend_graph();
    frontend_graph(frontend_graph&&) noexcept;
    auto operator=(frontend_graph&&) noexcept -> frontend_graph&;
    frontend_graph(const frontend_graph&) = delete;
    auto operator=(const frontend_graph&) -> frontend_graph& = delete;

    static auto open(std::filesystem::path state_directory,
                     std::filesystem::path working_tree,
                     std::shared_ptr<frontend_module> module,
                     frontend_action_executor execute) -> runtime::task<frontend_graph>;
    auto build(frontend_build_request request) -> runtime::task<frontend_build_report>;
    auto symbols(frontend_build_request request) -> runtime::task<symbol_record>;
    auto parse(frontend_parse_request request) -> runtime::task<parse_tree>;
    [[nodiscard]] auto language_for_path(std::string_view path) const -> result<frontend_language>;
    [[nodiscard]] auto parse_count() const -> std::uint64_t;
    auto mark_changed(std::string path, durability tier = durability::low) -> result<std::uint64_t>;
    [[nodiscard]] auto statistics() const -> graph_statistics;

private:
    struct impl;
    explicit frontend_graph(std::unique_ptr<impl> implementation);
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
