#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/object.hpp"

namespace liquid::objects {

struct repo_record {
    node_id repo_id{};
    std::string name;
    std::int64_t created{};
    std::optional<repo_ref> root_change;
};

[[nodiscard]] auto encode(const blob &value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const tree &value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const change &value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto encode(const repo_record &value) -> result<std::vector<std::byte>>;

[[nodiscard]] auto decode_blob(std::span<const std::byte> bytes) -> result<blob>;
[[nodiscard]] auto decode_tree(std::span<const std::byte> bytes) -> result<tree>;
[[nodiscard]] auto decode_change(std::span<const std::byte> bytes) -> result<change>;
[[nodiscard]] auto decode_repo(std::span<const std::byte> bytes) -> result<repo_record>;
[[nodiscard]] auto digest_of(std::span<const std::byte> bytes) -> result<digest>;

} // namespace liquid::objects
