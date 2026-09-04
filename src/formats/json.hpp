#pragma once

#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::formats::json {

struct null {
    auto operator==(const null&) const -> bool = default;
};

struct number {
    std::string text;

    auto operator==(const number&) const -> bool = default;
};

struct value {
    using array = std::vector<value>;
    using object = std::vector<std::pair<std::string, value>>;
    using storage = std::variant<null, bool, number, std::string, array, object>;

    storage data;

    value() : data(null{}) {}
    value(null item) : data(item) {}
    value(bool item) : data(item) {}
    value(number item) : data(std::move(item)) {}
    value(std::string item) : data(std::move(item)) {}
    value(array item) : data(std::move(item)) {}
    value(object item) : data(std::move(item)) {}

    auto operator==(const value&) const -> bool = default;
};

[[nodiscard]] auto parse(std::string_view input) -> result<value>;
[[nodiscard]] auto write(const value& input) -> result<std::string>;

} // namespace liquid::formats::json
