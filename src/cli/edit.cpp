#include <charconv>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/daemon.hpp"
#include "liquid/edit.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr,
                 "error: usage: liquid edit rename <path>:<line>:<column> <new-name> [options]");
    std::println(stderr,
                 "       liquid edit extract <path>:<start-line>-<end-line> <name> [--value] "
                 "[options]");
    std::println(stderr,
                 "       liquid edit move <path>:<line>:<column> <to-path> [--at <line>] "
                 "[options]");
    std::println(stderr, "       liquid edit apply <edits.cbor> [options]");
    return 2;
}

auto parse_unsigned(std::string_view text, std::string_view context)
    -> liquid::result<std::uint64_t> {
    auto output = std::uint64_t{};
    const auto [end, code] = std::from_chars(text.data(), text.data() + text.size(), output);
    if (code != std::errc{} || end != text.data() + text.size() || output == 0) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument, "expected a positive integer", std::string{context}});
    }
    return output;
}

auto parse_location(std::string_view text) -> liquid::result<liquid::edit_source_location> {
    const auto column_separator = text.rfind(':');
    const auto line_separator = column_separator == std::string_view::npos
                                    ? std::string_view::npos
                                    : text.rfind(':', column_separator - 1);
    if (line_separator == std::string_view::npos || column_separator == std::string_view::npos ||
        line_separator == 0) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "location must be path:line:column",
                                             std::string{text}});
    }
    auto line = parse_unsigned(
        text.substr(line_separator + 1, column_separator - line_separator - 1), "edit line");
    auto column = parse_unsigned(text.substr(column_separator + 1), "edit column");
    if (!line || !column) {
        return std::unexpected(line ? column.error() : line.error());
    }
    return liquid::edit_source_location{
        std::string{text.substr(0, line_separator)}, *line, *column};
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

auto range_json(const liquid::frontend_source_range& range) -> std::string {
    return std::format("{{\"start\":{{\"byte\":{},\"line\":{},\"column\":{}}},"
                       "\"end\":{{\"byte\":{},\"line\":{},\"column\":{}}}}}",
                       range.start.byte,
                       range.start.line,
                       range.start.column,
                       range.end.byte,
                       range.end.line,
                       range.end.column);
}

auto preview_json(const liquid::edit_preview& preview) -> std::string {
    auto changes = std::string{"["};
    for (std::size_t file_index = 0; file_index < preview.changes.size(); ++file_index) {
        const auto& file = preview.changes[file_index];
        if (file_index != 0) {
            changes += ',';
        }
        changes += std::format("{{\"path\":{},\"before\":{},\"after\":{},\"changes\":[",
                               json_string(file.path),
                               json_string(liquid::to_string(file.before)),
                               json_string(liquid::to_string(file.after)));
        for (std::size_t change_index = 0; change_index < file.changes.size(); ++change_index) {
            const auto& change = file.changes[change_index];
            if (change_index != 0) {
                changes += ',';
            }
            changes += std::format("{{\"range\":{},\"old_text\":{},\"new_text\":{}}}",
                                   range_json(change.range),
                                   json_string(change.old_text),
                                   json_string(change.new_text));
        }
        changes += "]}";
    }
    changes += ']';
    auto refused = std::string{"["};
    for (std::size_t index = 0; index < preview.refused.size(); ++index) {
        const auto& refusal = preview.refused[index];
        if (index != 0) {
            refused += ',';
        }
        refused += std::format("{{\"path\":{},\"kind\":{},\"range\":{},\"reason\":{}}}",
                               json_string(refusal.target.path),
                               json_string(refusal.target.node.kind),
                               range_json(refusal.target.node.range),
                               json_string(refusal.reason));
    }
    refused += ']';
    return std::format("{{\"v\":1,\"id\":{},\"changes\":{},\"refused\":{},\"attribution\":{{"
                       "\"tool\":{},\"operation\":{},\"agent\":{}}}}}",
                       json_string(liquid::to_string(preview.id)),
                       changes,
                       refused,
                       json_string(preview.attribution.tool),
                       json_string(preview.attribution.operation),
                       preview.attribution.agent ? json_string(*preview.attribution.agent)
                                                 : "null");
}

void print_diff(const liquid::edit_preview& preview) {
    for (const auto& file : preview.changes) {
        std::println("--- a/{}", file.path);
        std::println("+++ b/{}", file.path);
        for (const auto& change : file.changes) {
            std::println("@@ -{},{} +{},{} @@",
                         change.range.start.line,
                         change.range.end.line - change.range.start.line + 1,
                         change.range.start.line,
                         change.range.end.line - change.range.start.line + 1);
            if (!change.old_text.empty()) {
                std::println("-{}", change.old_text);
            }
            if (!change.new_text.empty()) {
                std::println("+{}", change.new_text);
            }
        }
    }
    for (const auto& refusal : preview.refused) {
        std::println(stderr,
                     "refused: {}:{}:{}: {}",
                     refusal.target.path,
                     refusal.target.node.range.start.line,
                     refusal.target.node.range.start.column,
                     refusal.reason);
    }
}

auto repository_path(const std::filesystem::path& checkout)
    -> liquid::result<std::filesystem::path> {
    auto cache = liquid::build::wrap_cache_directory(checkout);
    if (!cache) {
        return std::unexpected(cache.error());
    }
    const auto build_repository = *cache / "repository";
    if (std::filesystem::exists(build_repository / "LIQUID_REPO")) {
        return build_repository;
    }
    return checkout;
}

auto file_range(const std::filesystem::path& checkout,
                std::string path,
                std::uint64_t first_line,
                std::uint64_t last_line) -> liquid::result<liquid::frontend_source_range> {
    if (last_line < first_line) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, "extract line range is reversed", path});
    }
    auto location = repository_path(checkout);
    if (!location) {
        return std::unexpected(location.error());
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(*location));
    if (!repository) {
        return std::unexpected(repository.error());
    }
    auto contents = liquid::runtime::block_on(repository->read(path));
    if (!contents) {
        return std::unexpected(contents.error());
    }
    const auto source =
        std::string_view{reinterpret_cast<const char*>(contents->data()), contents->size()};
    const auto offset = [&](std::uint64_t line) -> std::optional<std::uint64_t> {
        if (line == 1) {
            return std::uint64_t{};
        }
        auto current = std::uint64_t{1};
        for (auto index = std::uint64_t{}; index < source.size(); ++index) {
            if (source[index] == '\n' && ++current == line) {
                return index + 1;
            }
        }
        if (current + 1 == line) {
            return source.size();
        }
        return std::nullopt;
    };
    auto start = offset(first_line);
    auto end = offset(last_line + 1);
    if (!start || !end) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument, "extract line lies outside the file", path});
    }
    return liquid::frontend_source_range{{*start, first_line, 1}, {*end, last_line + 1, 1}};
}

auto parse_extract_location(const std::filesystem::path& checkout, std::string_view text)
    -> liquid::result<std::pair<std::string, liquid::frontend_source_range>> {
    const auto separator = text.rfind(':');
    const auto dash = separator == std::string_view::npos ? std::string_view::npos
                                                          : text.find('-', separator + 1);
    if (separator == std::string_view::npos || dash == std::string_view::npos || separator == 0) {
        return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                             "selection must be path:start-line-end-line",
                                             std::string{text}});
    }
    auto first = parse_unsigned(text.substr(separator + 1, dash - separator - 1), "extract start");
    auto last = parse_unsigned(text.substr(dash + 1), "extract end");
    if (!first || !last) {
        return std::unexpected(first ? last.error() : first.error());
    }
    auto path = std::string{text.substr(0, separator)};
    auto range = file_range(checkout, path, *first, *last);
    if (!range) {
        return std::unexpected(range.error());
    }
    return std::pair{std::move(path), *range};
}

auto read_request_file(const std::filesystem::path& path) -> liquid::result<liquid::edit_request> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot open edit request", path.string()});
    }
    const auto chars = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto bytes = std::vector<std::byte>{};
    bytes.reserve(chars.size());
    for (const auto value : chars) {
        bytes.push_back(std::byte{static_cast<unsigned char>(value)});
    }
    return liquid::decode_edit_request(bytes);
}

} // namespace

auto edit(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage();
    }
    const auto subcommand = std::string_view{argv[0]};
    auto operands = std::vector<std::string>{};
    auto apply = false;
    auto json = false;
    auto value = false;
    auto at_line = std::optional<std::uint64_t>{};
    auto agent = std::optional<std::string>{};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--apply") {
            apply = true;
        } else if (argument == "--json") {
            json = true;
        } else if (argument == "--value") {
            value = true;
        } else if (argument == "--agent" && index + 1 < argc) {
            agent = argv[++index];
        } else if (argument == "--at" && index + 1 < argc) {
            auto parsed = parse_unsigned(argv[++index], "move line");
            if (!parsed) {
                return fail(parsed.error());
            }
            at_line = *parsed;
        } else if (argument.starts_with("--")) {
            return usage();
        } else {
            operands.emplace_back(argument);
        }
    }
    const auto checkout = std::filesystem::current_path();
    auto request = liquid::result<liquid::edit_request>{};
    auto operation = std::string{subcommand};
    if (subcommand == "rename" && operands.size() == 2 && !value && !at_line) {
        auto location = parse_location(operands[0]);
        if (!location) {
            return fail(location.error());
        }
        request = liquid::edit_request{liquid::rename_request{*location, operands[1]}};
    } else if (subcommand == "extract" && operands.size() == 2 && !at_line) {
        auto location = parse_extract_location(checkout, operands[0]);
        if (!location) {
            return fail(location.error());
        }
        request = liquid::edit_request{liquid::extract_request{
            location->first,
            location->second,
            operands[1],
            value ? liquid::extract_kind::value : liquid::extract_kind::function}};
    } else if (subcommand == "move" && operands.size() == 2 && !value) {
        auto location = parse_location(operands[0]);
        if (!location) {
            return fail(location.error());
        }
        request = liquid::edit_request{liquid::move_request{*location, operands[1], at_line}};
    } else if (subcommand == "apply" && operands.size() == 1 && !value && !at_line) {
        request = read_request_file(operands[0]);
        operation = "tree_edit";
    } else {
        return usage();
    }
    if (!request) {
        return fail(request.error());
    }
    auto ready = ensure_daemon(checkout);
    if (!ready) {
        return fail(ready.error());
    }
    auto attached = liquid::runtime::block_on(liquid::daemon::attach(checkout));
    if (!attached) {
        return fail(attached.error());
    }
    auto preview = liquid::runtime::block_on(
        attached->preview_edit(std::move(*request), {"liquid", operation, std::move(agent)}));
    if (!preview) {
        return fail(preview.error());
    }
    if (json) {
        std::println("{}", preview_json(*preview));
    } else {
        print_diff(*preview);
    }
    if (!preview->refused.empty()) {
        return 1;
    }
    if (apply) {
        auto applied = liquid::runtime::block_on(attached->apply_edit(*preview));
        if (!applied) {
            return fail(applied.error());
        }
        if (!json) {
            std::println("applied {} as {}",
                         preview->changes.size(),
                         liquid::to_string(applied->operation.value));
        }
    }
    return 0;
}

} // namespace liquid_cli
