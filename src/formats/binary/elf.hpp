// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::formats::binary {

enum class image_kind : std::uint8_t { elf, macho, pe };
enum class symbol_kind : std::uint8_t { function, object };
enum class path_kind : std::uint8_t { install_name, needed_library, runtime_search_path };

struct path_reference {
    path_kind kind{path_kind::install_name};
    std::string value;
    std::uint64_t offset{};
};

struct section {
    std::string name;
    std::uint64_t offset{};
    std::uint64_t size{};
    std::uint64_t address{};
    std::uint64_t flags{};
    std::uint32_t type{};
};

struct symbol {
    std::string name;
    std::optional<std::string> version;
    symbol_kind kind{symbol_kind::function};
    std::uint64_t size{};
    std::string binding;
    std::string visibility;
    std::optional<std::string> signature;
};

struct image {
    image_kind kind{image_kind::elf};
    std::string architecture;
    bool shared_library{};
    std::optional<std::string> install_name;
    std::vector<section> sections;
    std::vector<std::string> symbol_names;
    std::vector<symbol> exported_symbols;
    std::vector<std::string> needed_libraries;
    std::vector<path_reference> path_references;
};

[[nodiscard]] auto inspect_elf(std::span<const std::byte> bytes) -> result<image>;

} // namespace liquid::formats::binary
