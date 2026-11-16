// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "liquid/drip.hpp"
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/frontend.hpp"
#include "liquid/hash.hpp"
#include "liquid/object.hpp"

namespace liquid {

struct LIQUID_EXPORT edit_node {
    std::string kind;
    frontend_source_range range;
    digest fingerprint;
};

struct LIQUID_EXPORT edit_target {
    std::string path;
    edit_node node;
};

enum class tree_edit_operation : std::uint8_t {
    replace_node = 1,
    insert_before = 2,
    insert_after = 3,
    delete_node = 4,
};

struct LIQUID_EXPORT tree_edit {
    edit_target target;
    tree_edit_operation operation{tree_edit_operation::replace_node};
    std::string text;
};

struct LIQUID_EXPORT edit_source_location {
    std::string path;
    std::uint64_t line{};
    std::uint64_t column{};
};

using edit_location = std::variant<std::string, edit_source_location>;

struct LIQUID_EXPORT rename_request {
    edit_location at;
    std::string new_name;
};

enum class extract_kind : std::uint8_t {
    function = 1,
    value = 2,
};

struct LIQUID_EXPORT extract_request {
    std::string path;
    frontend_source_range range;
    std::string name;
    extract_kind kind{extract_kind::function};
};

struct LIQUID_EXPORT move_request {
    edit_location at;
    std::string to_path;
    std::optional<std::uint64_t> at_line;
};

struct LIQUID_EXPORT tree_edit_request {
    std::vector<tree_edit> edits;
};

using edit_request = std::variant<rename_request, extract_request, move_request, tree_edit_request>;

struct LIQUID_EXPORT edit_attribution {
    std::string tool;
    std::string operation;
    std::optional<std::string> agent;
};

struct LIQUID_EXPORT text_change {
    frontend_source_range range;
    std::string old_text;
    std::string new_text;
};

struct LIQUID_EXPORT file_edit {
    std::string path;
    digest before;
    digest after;
    std::vector<text_change> changes;
};

struct LIQUID_EXPORT edit_refusal {
    edit_target target;
    std::string reason;
};

struct LIQUID_EXPORT edit_preview {
    std::vector<file_edit> changes;
    std::vector<edit_refusal> refused;
    edit_attribution attribution;
    digest id;
};

struct LIQUID_EXPORT edit_result {
    std::vector<drip_id> drips;
    op_id operation;
};

[[nodiscard]] LIQUID_EXPORT auto encode_edit_request(const edit_request& value)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto decode_edit_request(std::span<const std::byte> value)
    -> result<edit_request>;
[[nodiscard]] LIQUID_EXPORT auto encode_edit_preview(const edit_preview& value)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto decode_edit_preview(std::span<const std::byte> value)
    -> result<edit_preview>;
[[nodiscard]] LIQUID_EXPORT auto encode_edit_result(const edit_result& value)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto decode_edit_result(std::span<const std::byte> value)
    -> result<edit_result>;

} // namespace liquid
