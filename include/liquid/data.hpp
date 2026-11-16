// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT session_id {
    std::array<std::byte, 16> bytes{};

    auto operator==(const session_id&) const -> bool = default;
};

[[nodiscard]] LIQUID_EXPORT std::string to_string(const session_id& value);
[[nodiscard]] LIQUID_EXPORT result<session_id> parse_session_id(std::string_view text);

enum class data_branch_mechanism : std::uint8_t {
    template_copy,
    reflink,
    automatic,
};

struct LIQUID_EXPORT data_database {
    std::string name;
    std::string url_env{"DATABASE_URL"};
    std::string base_url;
    data_branch_mechanism mechanism{data_branch_mechanism::automatic};

    auto operator==(const data_database&) const -> bool = default;
};

struct LIQUID_EXPORT data_config {
    std::vector<data_database> databases;

    auto operator==(const data_config&) const -> bool = default;
};

struct LIQUID_EXPORT data_branch_record {
    std::string id;
    session_id session;
    std::string database;
    std::string url;
    data_branch_mechanism mechanism{data_branch_mechanism::template_copy};
    std::chrono::sys_seconds created_at{};

    auto operator==(const data_branch_record&) const -> bool = default;
};

[[nodiscard]] LIQUID_EXPORT std::string_view to_string(data_branch_mechanism mechanism) noexcept;

namespace data_branch {

[[nodiscard]] LIQUID_EXPORT runtime::task<data_branch_record>
create(repository& source, session_id session, data_database database);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<data_branch_record>> list(repository& source);
[[nodiscard]] LIQUID_EXPORT runtime::task<void> discard(repository& source, std::string id);
[[nodiscard]] LIQUID_EXPORT runtime::task<void> promote(repository& source, std::string id);

} // namespace data_branch
} // namespace liquid
