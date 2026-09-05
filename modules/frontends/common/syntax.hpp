#pragma once

#include <cstdint>
#include <span>
#include <string>
#include <vector>

#include "cbor.hpp"

namespace liquid_frontend {

enum class syntax_language { c, cpp };

struct source_point {
    std::uint64_t byte{};
    std::uint64_t line{};
    std::uint64_t column{};
};

struct syntax_unit {
    std::uint64_t kind{};
    std::string name;
    source_point start;
    source_point end;
    std::uint64_t body_start{};
    std::uint64_t body_end{};
    std::vector<std::uint8_t> source;
};

auto parse_units(syntax_language language, std::span<const std::uint8_t> source)
    -> std::vector<syntax_unit>;
auto encode_units(const std::vector<syntax_unit>& units) -> std::vector<std::uint8_t>;

} // namespace liquid_frontend
