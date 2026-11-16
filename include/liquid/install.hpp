// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

enum class service_restart : std::uint8_t {
    no,
    on_failure,
    always,
};

enum class install_cache : std::uint8_t {
    ldconfig,
    desktop_database,
    mime_database,
    man_db,
    icon_cache,
    systemd_daemon_reload,
    fonts,
};

struct LIQUID_EXPORT install_service {
    std::string name;
    std::filesystem::path executable;
    std::string user;
    std::vector<std::string> after;
    service_restart restart{service_restart::on_failure};

    auto operator==(const install_service&) const -> bool = default;
};

struct LIQUID_EXPORT install_user {
    std::string name;
    bool system{true};
    std::filesystem::path home;

    auto operator==(const install_user&) const -> bool = default;
};

struct LIQUID_EXPORT install_group {
    std::string name;
    bool system{true};

    auto operator==(const install_group&) const -> bool = default;
};

struct LIQUID_EXPORT install_directory {
    std::filesystem::path path;
    std::uint32_t mode{0755};
    std::string owner{"root"};

    auto operator==(const install_directory&) const -> bool = default;
};

struct LIQUID_EXPORT install_symlink {
    std::filesystem::path path;
    std::filesystem::path target;

    auto operator==(const install_symlink&) const -> bool = default;
};

struct LIQUID_EXPORT install_alternative {
    std::string name;
    std::filesystem::path path;
    std::int32_t priority{};

    auto operator==(const install_alternative&) const -> bool = default;
};

struct LIQUID_EXPORT install_intent {
    std::uint64_t v{1};
    std::vector<install_service> services;
    std::vector<install_user> users;
    std::vector<install_group> groups;
    std::vector<install_directory> directories;
    std::vector<install_symlink> symlinks;
    std::vector<install_cache> caches;
    std::vector<install_alternative> alternatives;

    auto operator==(const install_intent&) const -> bool = default;
};

struct LIQUID_EXPORT install_generation {
    std::uint64_t number{};
    std::filesystem::path path;
    bool current{};
};

struct LIQUID_EXPORT install_report {
    install_generation generation;
    std::uint64_t files{};
    std::uint64_t linked{};
};

[[nodiscard]] LIQUID_EXPORT result<void> validate_install_intent(const install_intent& value);
[[nodiscard]] LIQUID_EXPORT result<std::vector<std::byte>>
encode_install_intent(const install_intent& value);
[[nodiscard]] LIQUID_EXPORT result<install_intent>
decode_install_intent(std::span<const std::byte> bytes);
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(service_restart value) noexcept;
[[nodiscard]] LIQUID_EXPORT std::string_view to_string(install_cache value) noexcept;

namespace installation {

[[nodiscard]] LIQUID_EXPORT runtime::task<install_report> install(std::filesystem::path source,
                                                                  std::filesystem::path root);
[[nodiscard]] LIQUID_EXPORT runtime::task<install_generation> rollback(std::filesystem::path root);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<install_generation>>
list(std::filesystem::path root);

} // namespace installation
} // namespace liquid
