// SPDX-License-Identifier: MIT

#pragma once

#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"

namespace liquid {

enum class abi_binary_kind : std::uint8_t { elf, macho, pe };
enum class abi_symbol_kind : std::uint8_t { function, object };

struct LIQUID_EXPORT abi_symbol {
    std::string name;
    std::optional<std::string> version;
    abi_symbol_kind kind{abi_symbol_kind::function};
    std::uint64_t size{};
    std::string binding;
    std::string visibility;
    std::optional<std::string> signature;

    auto operator==(const abi_symbol&) const -> bool = default;
};

struct LIQUID_EXPORT abi_descriptor {
    digest subject{};
    abi_binary_kind binary{abi_binary_kind::elf};
    std::string architecture;
    std::optional<std::string> install_name;
    std::vector<abi_symbol> exported_symbols;
    std::vector<std::string> needed_libraries;

    auto operator==(const abi_descriptor& other) const -> bool {
        return subject.alg == other.subject.alg && subject.bytes == other.subject.bytes &&
               binary == other.binary && architecture == other.architecture &&
               install_name == other.install_name && exported_symbols == other.exported_symbols &&
               needed_libraries == other.needed_libraries;
    }
};

enum class abi_verdict : std::uint8_t { compatible, compatible_additions, incompatible };
enum class abi_change_kind : std::uint8_t {
    added,
    removed,
    type_changed,
    size_changed,
    version_changed,
};

struct LIQUID_EXPORT abi_change {
    std::string symbol;
    abi_change_kind kind{abi_change_kind::added};
    std::string detail;

    auto operator==(const abi_change&) const -> bool = default;
};

struct LIQUID_EXPORT abi_report {
    abi_verdict verdict{abi_verdict::compatible};
    std::vector<abi_change> changes;

    auto operator==(const abi_report&) const -> bool = default;
};

[[nodiscard]] LIQUID_EXPORT result<abi_descriptor> extract_abi(std::span<const std::byte> binary);
[[nodiscard]] LIQUID_EXPORT abi_report diff_abi(const abi_descriptor& old_descriptor,
                                                const abi_descriptor& new_descriptor);
[[nodiscard]] LIQUID_EXPORT result<std::string> abi_json(const abi_descriptor& descriptor);
[[nodiscard]] LIQUID_EXPORT result<std::string> abi_json(const abi_report& report);

} // namespace liquid
