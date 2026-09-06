#include <algorithm>
#include <cstddef>
#include <filesystem>
#include <format>
#include <memory>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "liquid/frontend.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

auto frontend_module_path(bool compiler) -> std::filesystem::path;
auto print_tier(unsigned level, std::string_view mechanism) -> void;
auto tier_json(unsigned level, std::string_view mechanism) -> std::string;

namespace {

auto json_string(std::string_view value) -> std::string;

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: diff <a> <b> [path] [--text] [--formats] [--structural] [--json]");
    return 2;
}

auto tier_name(liquid::diff_tier tier) -> std::string_view {
    switch (tier) {
    case liquid::diff_tier::structural:
        return "structural";
    case liquid::diff_tier::text:
        return "text";
    case liquid::diff_tier::binary:
        return "binary";
    }
    return "binary";
}

auto structural_kind_name(liquid::structural_edit_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::structural_edit_kind::added:
        return "added";
    case liquid::structural_edit_kind::removed:
        return "removed";
    case liquid::structural_edit_kind::modified:
        return "modified";
    case liquid::structural_edit_kind::moved:
        return "moved";
    case liquid::structural_edit_kind::comment_changed:
        return "comment_changed";
    }
    return "modified";
}

auto range_json(const std::optional<liquid::frontend_source_range>& range) -> std::string {
    if (!range) {
        return "null";
    }
    return std::format("{{\"start\":{{\"byte\":{},\"line\":{},\"column\":{}}},"
                       "\"end\":{{\"byte\":{},\"line\":{},\"column\":{}}}}}",
                       range->start.byte,
                       range->start.line,
                       range->start.column,
                       range->end.byte,
                       range->end.line,
                       range->end.column);
}

auto structural_json(const liquid::structural_diff& value) -> std::string {
    auto edits = std::string{"["};
    for (auto index = std::size_t{}; index < value.edits.size(); ++index) {
        const auto& edit = value.edits[index];
        edits += index == 0 ? "" : ",";
        edits += std::format("{{\"kind\":{},\"node_kind\":{},\"name\":{},\"old_range\":{},"
                             "\"new_range\":{}}}",
                             json_string(structural_kind_name(edit.kind)),
                             json_string(edit.node_kind),
                             edit.name ? json_string(*edit.name) : "null",
                             range_json(edit.old_range),
                             range_json(edit.new_range));
    }
    edits += ']';
    return std::format("{{\"language\":{},\"grammar\":{},\"formatting_only\":{},\"edits\":{}}}",
                       json_string(value.language),
                       json_string(value.grammar),
                       value.formatting_only ? "true" : "false",
                       edits);
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
            if (static_cast<unsigned char>(character) < 0x20U) {
                output += std::format("\\u{:04x}", static_cast<unsigned char>(character));
            } else {
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto change_name(liquid::tree_diff_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::tree_diff_kind::added:
        return "added";
    case liquid::tree_diff_kind::removed:
        return "removed";
    case liquid::tree_diff_kind::modified:
        return "modified";
    case liquid::tree_diff_kind::moved:
        return "moved";
    case liquid::tree_diff_kind::mode_changed:
        return "mode_changed";
    case liquid::tree_diff_kind::conflicted:
        return "conflicted";
    }
    return "unknown";
}

auto container_change_name(liquid::container_diff_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::container_diff_kind::added:
        return "added";
    case liquid::container_diff_kind::removed:
        return "removed";
    case liquid::container_diff_kind::modified:
        return "modified";
    }
    return "unknown";
}

auto digest_json(const std::optional<liquid::digest>& value) -> std::string {
    return value ? json_string(liquid::to_string(*value)) : "null";
}

auto container_json(const liquid::container_diff& value) -> std::string {
    auto output = std::format("{{\"codec\":{},\"tier\":{},\"truncated\":{},\"entries\":[",
                              json_string(value.codec),
                              static_cast<unsigned>(value.tier),
                              value.truncated ? "true" : "false");
    for (auto index = std::size_t{}; index < value.entries.size(); ++index) {
        const auto& entry = value.entries[index];
        output += index == 0 ? "" : ",";
        output += std::format(
            "{{\"path\":{},\"kind\":{},\"old_digest\":{},\"new_digest\":{},\"nested\":{}}}",
            json_string(entry.path),
            json_string(container_change_name(entry.kind)),
            digest_json(entry.old_digest),
            digest_json(entry.new_digest),
            entry.nested ? container_json(*entry.nested) : "null");
    }
    output += "]}";
    return output;
}

auto entry_tier(const liquid::tree_diff_entry& entry, bool text)
    -> std::pair<unsigned, std::string> {
    if (entry.container) {
        return {static_cast<unsigned>(entry.container->tier),
                "format(" + entry.container->codec + ")"};
    }
    return {0, text ? "text" : "chunking"};
}

auto entry_json(const liquid::tree_diff_entry& entry, bool text) -> std::string {
    auto changes = std::string{"["};
    for (auto index = std::size_t{}; index < entry.changes.size(); ++index) {
        changes += index == 0 ? "" : ",";
        changes += json_string(change_name(entry.changes[index]));
    }
    changes += ']';
    const auto [level, mechanism] = entry_tier(entry, text);
    return std::format("{{\"path\":{},\"tier\":{},\"diff_tier\":{},\"tier_reason\":{},"
                       "\"changes\":{},\"structural\":{},\"container\":{}}}",
                       json_string(entry.path),
                       tier_json(level, mechanism),
                       json_string(tier_name(entry.tier)),
                       json_string(entry.tier_reason),
                       changes,
                       entry.structural ? structural_json(*entry.structural) : "null",
                       entry.container ? container_json(*entry.container) : "null");
}

auto print_container(const liquid::container_diff& value, std::size_t indent) -> void {
    for (const auto& entry : value.entries) {
        std::println("{:{}}{} {}", "", indent, container_change_name(entry.kind), entry.path);
        if (entry.nested) {
            std::println("{:{}}{} tier={}",
                         "",
                         indent + 2,
                         entry.nested->codec,
                         static_cast<unsigned>(entry.nested->tier));
            print_container(*entry.nested, indent + 4);
        }
    }
    if (value.truncated) {
        std::println("{:{}}truncated", "", indent);
    }
}

auto print_entry(const liquid::tree_diff_entry& entry, bool text, bool structural) -> void {
    if (entry.structural) {
        if (entry.structural->formatting_only) {
            std::println("reformat only {}", entry.path);
        } else {
            for (const auto& edit : entry.structural->edits) {
                auto node_kind = edit.node_kind;
                if (node_kind.find("function") != std::string::npos) {
                    node_kind = "function";
                }
                const auto name = edit.name ? " " + *edit.name : std::string{};
                if (edit.kind == liquid::structural_edit_kind::moved && edit.old_range &&
                    edit.new_range) {
                    std::println("moved {}{}: {}-{} -> {}-{}",
                                 node_kind,
                                 name,
                                 edit.old_range->start.line,
                                 edit.old_range->end.line,
                                 edit.new_range->start.line,
                                 edit.new_range->end.line);
                } else {
                    std::println("{} {}{}", structural_kind_name(edit.kind), node_kind, name);
                }
            }
        }
        print_tier(2, "syntax tree");
        return;
    }
    const auto [level, mechanism] = entry_tier(entry, text);
    if (entry.container) {
        std::println("{} {} tier={}",
                     entry.path,
                     entry.container->codec,
                     static_cast<unsigned>(entry.container->tier));
        print_container(*entry.container, 2);
        print_tier(level, mechanism);
        return;
    }
    if (structural && !entry.tier_reason.empty()) {
        std::println("{} ({}) {}", tier_name(entry.tier), entry.tier_reason, entry.path);
        print_tier(level, mechanism);
        return;
    }
    auto changes = std::string{};
    for (const auto change : entry.changes) {
        if (!changes.empty()) {
            changes += ',';
        }
        changes += change_name(change);
    }
    std::println("{} {}", changes, entry.path);
    print_tier(level, mechanism);
}

auto resolve_change(std::span<const liquid::ref> refs, std::string_view value)
    -> liquid::result<liquid::change_id> {
    const auto named = std::ranges::find(refs, value, &liquid::ref::name);
    if (named != refs.end()) {
        return named->id;
    }
    auto parsed = liquid::parse_digest(value);
    if (!parsed) {
        return std::unexpected(
            liquid::error{liquid::errc::not_found, "diff reference is absent", std::string{value}});
    }
    return liquid::change_id{*parsed};
}

auto selected_path(std::string_view candidate, const std::string& path) -> bool {
    return path.empty() || candidate == path ||
           (candidate.size() > path.size() && candidate.starts_with(path) &&
            candidate[path.size()] == '/');
}

} // namespace

auto diff(int argc, char** argv) -> int {
    auto positional = std::vector<std::string>{};
    auto options = liquid::tree_diff_options{};
    auto json = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--text") {
            options.text = true;
        } else if (argument == "--formats") {
            options.formats = true;
        } else if (argument == "--structural") {
            options.structural = true;
        } else if (argument == "--json") {
            json = true;
        } else if (argument.starts_with("--")) {
            return usage();
        } else {
            positional.emplace_back(argument);
        }
    }
    if (positional.size() < 2 || positional.size() > 3) {
        return usage();
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto frontend = std::optional<liquid::frontend_graph>{};
    if (options.structural) {
        auto loaded = liquid::frontend_module::load(frontend_module_path(false));
        if (!loaded) {
            return fail(loaded.error());
        }
        auto module = std::make_shared<liquid::frontend_module>(std::move(*loaded));
        auto opened = liquid::runtime::block_on(liquid::frontend_graph::open(
            std::filesystem::current_path() / ".liquid" / "structural-diff-graph",
            std::filesystem::current_path(),
            std::move(module),
            [](std::vector<std::byte>) -> liquid::runtime::task<std::vector<std::byte>> {
                co_return std::unexpected(liquid::error{liquid::errc::unsupported,
                                                        "syntax frontend cannot execute actions",
                                                        "structural diff"});
            }));
        if (!opened) {
            return fail(opened.error());
        }
        frontend = std::move(*opened);
        options.frontend = &*frontend;
    }
    auto refs = liquid::runtime::block_on(repository->refs());
    if (!refs) {
        return fail(refs.error());
    }
    auto old_change = resolve_change(*refs, positional[0]);
    auto new_change = resolve_change(*refs, positional[1]);
    if (!old_change) {
        return fail(old_change.error());
    }
    if (!new_change) {
        return fail(new_change.error());
    }
    auto entries = liquid::runtime::block_on(repository->diff(*old_change, *new_change, options));
    if (!entries) {
        return fail(entries.error());
    }
    const auto path = positional.size() == 3 ? positional[2] : std::string{};
    if (json) {
        std::print("[");
    }
    auto emitted = std::size_t{};
    for (const auto& entry : *entries) {
        if (!selected_path(entry.path, path)) {
            continue;
        }
        if (json) {
            std::print("{}{}", emitted == 0 ? "" : ",", entry_json(entry, options.text));
        } else {
            print_entry(entry, options.text, options.structural);
        }
        ++emitted;
    }
    if (json) {
        std::println("]");
    }
    return 0;
}

} // namespace liquid_cli
