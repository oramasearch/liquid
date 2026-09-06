#include "liquid/tiers.hpp"

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>

#include "liquid/frontend.hpp"

namespace liquid {
namespace {

auto language_row(frontend_language value) -> language_support {
    const auto frontend = value.tier_mechanism == frontend_tier_mechanism::frontend;
    auto mechanism =
        frontend ? "frontend(" + value.module + ")" : "grammar(" + value.grammar_digest + ")";
    return {.language = std::move(value.language),
            .extensions = std::move(value.extensions),
            .tier = static_cast<std::uint8_t>(frontend ? 3 : 2),
            .mechanism = std::move(mechanism),
            .notes = frontend ? "Compiler frontend module." : "Digest-locked tree-sitter grammar."};
}

auto format_row(const format_codec& codec) -> format_support {
    const auto& descriptor = codec.descriptor();
    return {.format = descriptor.name,
            .tier = static_cast<std::uint8_t>(descriptor.tier),
            .codec = descriptor.name,
            .limits = descriptor.limits};
}

auto platform_rows() -> std::vector<platform_support> {
    return {
        {.os = "Linux",
         .tier_1_status = "supported",
         .sandbox = "namespaces",
         .vfs = "FUSE",
         .notes = "Reference platform; libfuse3 is required."},
        {.os = "macOS",
         .tier_1_status = "requires macFUSE",
         .sandbox = "sandbox-exec",
         .vfs = "macFUSE",
         .notes = "Tier 1 is available when liquid is built with macFUSE."},
        {.os = "Windows",
         .tier_1_status = "unsupported",
         .sandbox = "unsupported",
         .vfs = "unsupported",
         .notes = "Storage and versioning remain available at Tier 0."},
        {.os = "BSD",
         .tier_1_status = "unsupported",
         .sandbox = "unsupported",
         .vfs = "unsupported",
         .notes = "No wrapper backend is registered."},
    };
}

} // namespace

auto support_matrix() -> matrix {
    auto output = matrix{};
    output.languages.push_back({.language = "*",
                                .extensions = {"*"},
                                .tier = 0,
                                .mechanism = "chunking",
                                .notes = "Every language and file type has this floor."});
    for (auto& language : frontend_registry::global().languages()) {
        output.languages.push_back(language_row(std::move(language)));
    }
    std::ranges::sort(output.languages, {}, &language_support::language);

    output.formats.push_back(
        {.format = "unknown", .tier = 3, .codec = "none", .limits = format_limits{}});
    for (const auto* codec : format_registry::global().codecs()) {
        output.formats.push_back(format_row(*codec));
    }
    std::ranges::sort(output.formats, {}, &format_support::format);
    output.platforms = platform_rows();
    return output;
}

} // namespace liquid
