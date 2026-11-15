// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <format>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/frontend.hpp"
#include "liquid/tiers.hpp"

namespace liquid_cli {

auto frontend_module_path(bool compiler) -> std::filesystem::path;

namespace {

enum class output_format : std::uint8_t { text, json, markdown };

struct tier_options {
    output_format format{output_format::text};
    std::string language;
    std::string format_name;
};

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: tiers [--json|--markdown] [--language <name>|--format <name>]");
    return 2;
}

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '"':
            output += "\\\"";
            break;
        case '\n':
            output += "\\n";
            break;
        case '\r':
            output += "\\r";
            break;
        case '\t':
            output += "\\t";
            break;
        default:
            output += character;
        }
    }
    output += '"';
    return output;
}

auto join(std::span<const std::string> values, std::string_view separator) -> std::string {
    auto output = std::string{};
    for (const auto& value : values) {
        if (!output.empty()) {
            output += separator;
        }
        output += value;
    }
    return output;
}

auto load_frontends() -> liquid::result<void> {
    for (const auto compiler : {false, true}) {
        const auto path = frontend_module_path(compiler);
        if (!std::filesystem::exists(path)) {
            continue;
        }
        auto loaded = liquid::frontend_registry::global().load(path);
        if (!loaded && loaded.error().code != liquid::errc::already_exists) {
            return std::unexpected(loaded.error());
        }
    }
    return {};
}

auto select(liquid::matrix value, const tier_options& options) -> liquid::matrix {
    if (!options.language.empty()) {
        std::erase_if(value.languages,
                      [&](const auto& row) { return row.language != options.language; });
        value.formats.clear();
        value.platforms.clear();
    }
    if (!options.format_name.empty()) {
        std::erase_if(value.formats,
                      [&](const auto& row) { return row.format != options.format_name; });
        value.languages.clear();
        value.platforms.clear();
    }
    return value;
}

auto print_languages_json(std::span<const liquid::language_support> rows) -> void {
    std::print("\"languages\":[");
    for (auto index = std::size_t{}; index < rows.size(); ++index) {
        const auto& row = rows[index];
        std::print("{}{{\"language\":{},\"extensions\":[",
                   index == 0 ? "" : ",",
                   json_string(row.language));
        for (auto extension = std::size_t{}; extension < row.extensions.size(); ++extension) {
            std::print("{}{}", extension == 0 ? "" : ",", json_string(row.extensions[extension]));
        }
        std::print("],\"tier\":{},\"mechanism\":{},\"notes\":{}}}",
                   row.tier,
                   json_string(row.mechanism),
                   json_string(row.notes));
    }
    std::print("]");
}

auto print_formats_json(std::span<const liquid::format_support> rows) -> void {
    std::print("\"formats\":[");
    for (auto index = std::size_t{}; index < rows.size(); ++index) {
        const auto& row = rows[index];
        std::print("{}{{\"format\":{},\"tier\":{},\"codec\":{},\"limits\":{{"
                   "\"maximum_depth\":{},\"byte_budget\":{},\"maximum_entries\":{}}}}}",
                   index == 0 ? "" : ",",
                   json_string(row.format),
                   row.tier,
                   json_string(row.codec),
                   row.limits.maximum_depth,
                   row.limits.byte_budget,
                   row.limits.maximum_entries);
    }
    std::print("]");
}

auto print_platforms_json(std::span<const liquid::platform_support> rows) -> void {
    std::print("\"platforms\":[");
    for (auto index = std::size_t{}; index < rows.size(); ++index) {
        const auto& row = rows[index];
        std::print("{}{{\"os\":{},\"tier_1_status\":{},\"sandbox\":{},\"vfs\":{},"
                   "\"notes\":{}}}",
                   index == 0 ? "" : ",",
                   json_string(row.os),
                   json_string(row.tier_1_status),
                   json_string(row.sandbox),
                   json_string(row.vfs),
                   json_string(row.notes));
    }
    std::print("]");
}

auto print_json(const liquid::matrix& value) -> void {
    std::print("{{");
    print_languages_json(value.languages);
    std::print(",");
    print_formats_json(value.formats);
    std::print(",");
    print_platforms_json(value.platforms);
    std::println("}}");
}

auto print_language_markdown(std::span<const liquid::language_support> languages,
                             std::span<const liquid::platform_support> platforms) -> void {
    std::println("<!-- tiers:begin -->");
    std::println("| Language | Extensions | Tier | Mechanism | Notes |");
    std::println("|---|---|---:|---|---|");
    for (const auto& row : languages) {
        std::println("| `{}` | {} | {} | `{}` | {} |",
                     row.language,
                     join(row.extensions, ", "),
                     row.tier,
                     row.mechanism,
                     row.notes);
    }
    if (!platforms.empty()) {
        std::println("");
        std::println("### Declared Tier 1 platforms");
        std::println("");
        std::println("| OS | Tier 1 status | Sandbox | VFS | Notes |");
        std::println("|---|---|---|---|---|");
        for (const auto& row : platforms) {
            std::println("| {} | {} | `{}` | `{}` | {} |",
                         row.os,
                         row.tier_1_status,
                         row.sandbox,
                         row.vfs,
                         row.notes);
        }
    }
    std::println("<!-- tiers:end -->");
}

auto print_format_markdown(std::span<const liquid::format_support> formats) -> void {
    std::println("<!-- formats:begin -->");
    std::println("| Format | Tier | Codec | Limits |");
    std::println("|---|---:|---|---|");
    for (const auto& row : formats) {
        std::println("| `{}` | {} | `{}` | depth {}; {} bytes; {} entries |",
                     row.format,
                     row.tier,
                     row.codec,
                     row.limits.maximum_depth,
                     row.limits.byte_budget,
                     row.limits.maximum_entries);
    }
    std::println("<!-- formats:end -->");
}

auto print_markdown(const liquid::matrix& value) -> void {
    if (!value.languages.empty() || !value.platforms.empty()) {
        print_language_markdown(value.languages, value.platforms);
    }
    if ((!value.languages.empty() || !value.platforms.empty()) && !value.formats.empty()) {
        std::println("");
    }
    if (!value.formats.empty()) {
        print_format_markdown(value.formats);
    }
}

auto print_text(const liquid::matrix& value) -> void {
    for (const auto& row : value.languages) {
        std::println("language={} extensions={} tier={} mechanism={} notes={}",
                     row.language,
                     join(row.extensions, ","),
                     row.tier,
                     row.mechanism,
                     row.notes);
    }
    for (const auto& row : value.formats) {
        std::println("format={} tier={} codec={} limits=depth:{},bytes:{},entries:{}",
                     row.format,
                     row.tier,
                     row.codec,
                     row.limits.maximum_depth,
                     row.limits.byte_budget,
                     row.limits.maximum_entries);
    }
    for (const auto& row : value.platforms) {
        std::println("os={} tier_1_status={} sandbox={} vfs={} notes={}",
                     row.os,
                     row.tier_1_status,
                     row.sandbox,
                     row.vfs,
                     row.notes);
    }
}

} // namespace

auto tiers(int argc, char** argv) -> int {
    auto options = tier_options{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            if (options.format != output_format::text) {
                return usage();
            }
            options.format = output_format::json;
        } else if (argument == "--markdown") {
            if (options.format != output_format::text) {
                return usage();
            }
            options.format = output_format::markdown;
        } else if (argument == "--language") {
            if (++index == argc || !options.language.empty() || !options.format_name.empty()) {
                return usage();
            }
            options.language = argv[index];
        } else if (argument == "--format") {
            if (++index == argc || !options.format_name.empty() || !options.language.empty()) {
                return usage();
            }
            options.format_name = argv[index];
        } else {
            return usage();
        }
    }
    auto loaded = load_frontends();
    if (!loaded) {
        return fail(loaded.error());
    }
    auto value = select(liquid::support_matrix(), options);
    if ((!options.language.empty() && value.languages.empty()) ||
        (!options.format_name.empty() && value.formats.empty())) {
        return fail({liquid::errc::not_found,
                     "support matrix entry is absent",
                     options.language.empty() ? options.format_name : options.language});
    }
    switch (options.format) {
    case output_format::json:
        print_json(value);
        break;
    case output_format::markdown:
        print_markdown(value);
        break;
    case output_format::text:
        print_text(value);
        break;
    }
    return 0;
}

} // namespace liquid_cli
