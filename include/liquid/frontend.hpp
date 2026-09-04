#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/frontend_abi.h"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

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

struct LIQUID_EXPORT frontend_callbacks {
    std::function<void(std::string_view, std::uint64_t, std::uint64_t)> progress;
    std::function<bool()> cancelled;
};

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

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

struct LIQUID_EXPORT frontend_build_request {
    std::string language;
    std::string source_path;
    std::vector<std::byte> source;
    std::vector<std::byte> context_cbor;
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
    auto mark_changed(std::string path, durability tier = durability::low) -> result<std::uint64_t>;
    [[nodiscard]] auto statistics() const -> graph_statistics;

private:
    struct impl;
    explicit frontend_graph(std::unique_ptr<impl> implementation);
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
