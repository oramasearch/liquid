// SPDX-License-Identifier: MIT

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto parse_change(std::string_view value) -> liquid::result<liquid::change_id> {
    auto parsed = liquid::parse_digest(value);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return liquid::change_id{*parsed};
}

auto parse_anchor(std::string_view value) -> liquid::result<liquid::text_anchor> {
    const auto at = value.rfind('@');
    const auto colon = at == std::string_view::npos ? std::string_view::npos : value.rfind(':', at);
    if (at == std::string_view::npos || colon == std::string_view::npos || colon == 0 ||
        colon + 1 == at || at + 1 == value.size()) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "anchor must be <path>:<line>@<change>",
                                             std::string{value}});
    }
    auto line = std::uint64_t{};
    const auto line_text = value.substr(colon + 1, at - colon - 1);
    const auto [end, code] =
        std::from_chars(line_text.data(), line_text.data() + line_text.size(), line);
    if (code != std::errc{} || end != line_text.data() + line_text.size() || line == 0) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "anchor line must be a positive integer",
                                             std::string{line_text}});
    }
    auto change = parse_change(value.substr(at + 1));
    if (!change) {
        return std::unexpected(change.error());
    }
    return liquid::text_anchor{
        .change = *change, .path = std::string{value.substr(0, colon)}, .line = line, .column = 1};
}

} // namespace

auto anchor(int argc, char** argv) -> int {
    if (argc != 3 || std::string_view{argv[1]} != "--to") {
        return usage("anchor requires <path>:<line>@<change> --to <change>");
    }
    auto source = parse_anchor(argv[0]);
    if (!source) {
        return fail(source.error());
    }
    auto target = parse_change(argv[2]);
    if (!target) {
        return fail(target.error());
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto mapped = liquid::runtime::block_on(repository->map_anchor(std::move(*source), *target));
    if (!mapped) {
        return fail(mapped.error());
    }
    std::println("{}:{}:{}@{} moved={} approximate={} deleted={}",
                 mapped->path,
                 mapped->line,
                 mapped->column,
                 liquid::to_string(mapped->change.value),
                 mapped->moved,
                 mapped->approximate,
                 mapped->deleted);
    return 0;
}

} // namespace liquid_cli
