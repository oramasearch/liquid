#pragma once

#include <cstdint>
#include <expected>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "cbor.hpp"

namespace liquid_frontend {

struct grammar_descriptor;

enum class syntax_language { c, cpp };

enum class syntax_error_code { unsupported, incompatible_abi, parse_failed, invalid_query };

struct syntax_error {
    syntax_error_code code;
    std::string message;
};

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

struct syntax_symbol {
    std::string symbol;
    std::string name;
    std::string kind;
    std::string role;
    source_point start;
    source_point end;
    std::string container;
};

auto parse_units(syntax_language language, std::span<const std::uint8_t> source)
    -> std::vector<syntax_unit>;
auto encode_units(const std::vector<syntax_unit>& units) -> std::vector<std::uint8_t>;
auto parse_symbols(syntax_language language, std::span<const std::uint8_t> source)
    -> std::vector<syntax_symbol>;
auto parse_units(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<std::vector<syntax_unit>, syntax_error>;
auto parse_symbols(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<std::vector<syntax_symbol>, syntax_error>;
auto parse_has_error(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<bool, syntax_error>;
auto parse_has_error(const grammar_descriptor& grammar, std::span<const std::uint8_t> source)
    -> std::expected<bool, syntax_error>;
auto encode_symbols(syntax_language language,
                    std::string_view origin,
                    const std::vector<syntax_symbol>& symbols) -> std::vector<std::uint8_t>;
auto encode_symbols(std::string_view language,
                    std::string_view origin,
                    std::string_view grammar_digest,
                    const std::vector<syntax_symbol>& symbols) -> std::vector<std::uint8_t>;
auto encode_syntax_tree(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<std::vector<std::uint8_t>, syntax_error>;
auto encode_parse_tree(std::string_view language, std::span<const std::uint8_t> source)
    -> std::expected<std::vector<std::uint8_t>, syntax_error>;
auto parse_units_from_value(std::span<const std::uint8_t> value)
    -> std::expected<std::vector<syntax_unit>, syntax_error>;
auto parse_units_bytes_from_value(std::span<const std::uint8_t> value)
    -> std::expected<std::vector<std::uint8_t>, syntax_error>;
auto parse_symbols_bytes_from_value(std::span<const std::uint8_t> value)
    -> std::expected<std::vector<std::uint8_t>, syntax_error>;

} // namespace liquid_frontend
