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

auto parse_change(std::string_view value) -> liquid::result<liquid::change_id> {
    auto parsed = liquid::parse_digest(value);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return liquid::change_id{*parsed};
}

auto kind_name(liquid::tree_diff_kind kind) -> std::string_view {
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
        return "mode-changed";
    case liquid::tree_diff_kind::conflicted:
        return "conflicted";
    }
    return "unknown";
}

auto kinds(const liquid::tree_diff_entry& entry) -> std::string {
    auto result = std::string{};
    for (const auto kind : entry.changes) {
        if (!result.empty()) {
            result += ',';
        }
        result += kind_name(kind);
    }
    return result;
}

} // namespace

auto repo_diff(int argc, char** argv) -> int {
    if (argc != 2) {
        std::println(stderr, "error: usage: diff <old-change> <new-change> ()");
        return 2;
    }
    auto old_change = parse_change(argv[0]);
    if (!old_change) {
        return fail(old_change.error());
    }
    auto new_change = parse_change(argv[1]);
    if (!new_change) {
        return fail(new_change.error());
    }
    auto repository =
        liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
    if (!repository) {
        return fail(repository.error());
    }
    auto difference = liquid::runtime::block_on(
        repository->diff(*old_change, *new_change, liquid::tree_diff_options{.text = true}));
    if (!difference) {
        return fail(difference.error());
    }
    for (const auto& entry : *difference) {
        if (entry.old_path) {
            std::println("{}\t{}\tfrom={}", kinds(entry), entry.path, *entry.old_path);
        } else {
            std::println("{}\t{}", kinds(entry), entry.path);
        }
        if (!entry.line_hunks) {
            continue;
        }
        for (const auto& hunk : *entry.line_hunks) {
            std::println("  @@ -{},{} +{},{} @@",
                         hunk.old_line,
                         hunk.old_count,
                         hunk.new_line,
                         hunk.new_count);
        }
    }
    return 0;
}

} // namespace liquid_cli
