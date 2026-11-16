// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::encoding::cbor {

struct null {
    auto operator==(const null &) const -> bool = default;
};

struct value {
    using bytes = std::vector<std::byte>;
    using array = std::vector<value>;
    using map = std::vector<std::pair<value, value>>;
    using storage =
        std::variant<null, bool, std::uint64_t, std::int64_t, bytes, std::string, array, map>;

    storage data;

    value() : data(null{}) {}
    value(null v) : data(v) {}
    value(bool v) : data(v) {}
    value(std::uint64_t v) : data(v) {}
    value(std::int64_t v) : data(v) {}
    value(bytes v) : data(std::move(v)) {}
    value(std::string v) : data(std::move(v)) {}
    value(const char *v) : data(std::string{v}) {}
    value(array v) : data(std::move(v)) {}
    value(map v) : data(std::move(v)) {}

    auto operator==(const value &) const -> bool = default;
};

[[nodiscard]] auto encode(const value &input) -> result<std::vector<std::byte>>;
[[nodiscard]] auto decode(const std::vector<std::byte> &input) -> result<value>;
[[nodiscard]] auto decode(std::span<const std::byte> input) -> result<value>;

} // namespace liquid::encoding::cbor
