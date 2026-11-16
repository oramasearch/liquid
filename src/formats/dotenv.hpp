// SPDX-License-Identifier: MIT

#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::formats::dotenv {

enum class line_kind { blank, comment, assignment };

struct line {
    line_kind kind{line_kind::blank};
    std::string text;
    std::string key;
    std::string value;
    bool exported{};

    auto operator==(const line&) const -> bool = default;
};

struct document {
    std::vector<line> lines;

    auto operator==(const document&) const -> bool = default;
};

[[nodiscard]] auto parse(std::string_view input) -> result<document>;
[[nodiscard]] auto write(const document& input) -> result<std::string>;

} // namespace liquid::formats::dotenv
