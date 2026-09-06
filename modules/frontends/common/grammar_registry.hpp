#pragma once

#include <cstdint>
#include <span>
#include <string_view>

#include <tree_sitter/api.h>

namespace liquid_frontend {

struct grammar_descriptor {
    std::string_view language;
    std::span<const std::string_view> extensions;
    const TSLanguage* (*language_function)();
    std::span<const std::uint8_t> tags_query;
    std::string_view node_types_digest;
    std::uint32_t abi_version;
};

auto available_grammars() -> std::span<const grammar_descriptor>;
auto find_grammar(std::string_view language) -> const grammar_descriptor*;
auto grammar_abi_supported(std::uint32_t version) -> bool;
auto grammar_lock_digest() -> std::string_view;

} // namespace liquid_frontend
