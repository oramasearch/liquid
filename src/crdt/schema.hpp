// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include <automerge-cpp/document.hpp>

#include "liquid/error.hpp"

namespace liquid::crdt {

using actor_id = std::array<std::byte, 16>;
using node_id = std::array<std::byte, 16>;
using repo_id = std::array<std::byte, 16>;

enum class operation_kind : std::uint8_t { write = 1, move = 2, remove = 3 };

struct operation {
    node_id id{};
    operation_kind kind{operation_kind::write};
    std::string value_kind{"blob"};
    std::string path;
    std::string to;
    std::string ref;
    std::uint32_t mode{0};
    node_id entry_id{};

    auto operator==(const operation&) const -> bool = default;
};

struct entry {
    std::string kind;
    node_id id{};
    std::uint32_t mode{0};
    std::string ref;

    auto operator==(const entry&) const -> bool = default;
};

class schema {
public:
    static auto initialize(automerge_cpp::Document& document, const repo_id& repository)
        -> result<void>;
    static auto validate(const automerge_cpp::Document& document,
                         const std::vector<operation>& operations) -> result<void>;
    static auto apply(automerge_cpp::Document& document,
                      const std::vector<operation>& operations,
                      const actor_id& actor,
                      std::string_view message,
                      std::int64_t time) -> result<void>;
    static auto inspect(const automerge_cpp::Document& document, std::string_view path)
        -> result<std::optional<entry>>;
    static auto entry_names(const automerge_cpp::Document& document)
        -> result<std::vector<std::string>>;
    static auto entries(const automerge_cpp::Document& document) -> result<std::vector<entry>>;
    static auto operations(const automerge_cpp::Document& document)
        -> result<std::vector<operation>>;
    static auto valid_root(const automerge_cpp::Document& document) -> result<bool>;
};

} // namespace liquid::crdt
