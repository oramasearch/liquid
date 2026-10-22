#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <print>
#include <ranges>
#include <string_view>
#include <vector>

#include "liquid/abi.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {}", message);
    return 2;
}

auto kind_name(liquid::abi_symbol_kind kind) -> std::string_view {
    return kind == liquid::abi_symbol_kind::function ? "function" : "object";
}

auto verdict_name(liquid::abi_verdict verdict) -> std::string_view {
    switch (verdict) {
    case liquid::abi_verdict::compatible:
        return "compatible";
    case liquid::abi_verdict::compatible_additions:
        return "compatible_additions";
    case liquid::abi_verdict::incompatible:
        return "incompatible";
    }
    return "incompatible";
}

auto change_name(liquid::abi_change_kind kind) -> std::string_view {
    switch (kind) {
    case liquid::abi_change_kind::added:
        return "added";
    case liquid::abi_change_kind::removed:
        return "removed";
    case liquid::abi_change_kind::type_changed:
        return "type_changed";
    case liquid::abi_change_kind::size_changed:
        return "size_changed";
    case liquid::abi_change_kind::version_changed:
        return "version_changed";
    case liquid::abi_change_kind::inspection_truncated:
        return "inspection_truncated";
    }
    return "type_changed";
}

auto read_file(const std::filesystem::path& path) -> liquid::result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    if (!input) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read ABI input", path.string()});
    }
    const auto end = input.tellg();
    if (end < 0) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot size ABI input", path.string()});
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(end));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    if (!input && !output.empty()) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read ABI input", path.string()});
    }
    return output;
}

auto open_build_repository(const std::filesystem::path& root)
    -> liquid::result<liquid::repository> {
    if (std::filesystem::exists(root / "LIQUID_REPO")) {
        return liquid::runtime::block_on(liquid::repository::open(root));
    }
    auto cache = liquid::build::wrap_cache_directory(root);
    if (!cache) {
        return std::unexpected(cache.error());
    }
    return liquid::runtime::block_on(liquid::repository::open(*cache / "repository"));
}

auto descriptor_for_result(liquid::repository& repository, std::string_view operand)
    -> liquid::result<std::optional<liquid::abi_descriptor>> {
    const auto first = operand.find(':');
    const auto second = first == std::string_view::npos ? first : operand.find(':', first + 1);
    if (second == std::string_view::npos) {
        return std::optional<liquid::abi_descriptor>{};
    }
    auto id = liquid::parse_digest(operand.substr(0, second));
    if (!id) {
        return std::optional<liquid::abi_descriptor>{};
    }
    const auto path = operand.substr(second + 1);
    auto results = liquid::runtime::block_on(
        repository.results({.id = liquid::result_id{*id}, .page = {.limit = 1}}));
    if (!results) {
        return std::unexpected(results.error());
    }
    if (results->records.empty()) {
        return std::unexpected(liquid::error{
            liquid::errc::not_found, "ABI result is not stored", liquid::to_string(*id)});
    }
    const auto output = std::ranges::find(
        results->records.front().value.outputs, path, &liquid::action_output::path);
    if (output == results->records.front().value.outputs.end()) {
        return std::unexpected(liquid::error{
            liquid::errc::not_found, "result does not produce ABI path", std::string{path}});
    }
    auto descriptor = liquid::runtime::block_on(repository.abi(output->blob));
    if (!descriptor) {
        return std::unexpected(descriptor.error());
    }
    return std::optional<liquid::abi_descriptor>{std::move(*descriptor)};
}

auto resolve(std::string_view operand) -> liquid::result<liquid::abi_descriptor> {
    const auto root = std::filesystem::current_path();
    auto repository = open_build_repository(root);
    if (repository) {
        auto result_operand = descriptor_for_result(*repository, operand);
        if (!result_operand) {
            return std::unexpected(result_operand.error());
        }
        if (*result_operand) {
            return std::move(**result_operand);
        }
        if (auto id = liquid::parse_digest(operand)) {
            return liquid::runtime::block_on(repository->abi(*id));
        }
        auto results = liquid::runtime::block_on(
            repository->results({.output = std::string{operand}, .page = {.limit = 1}}));
        if (!results) {
            return std::unexpected(results.error());
        }
        if (!results->records.empty()) {
            const auto found = std::ranges::find(
                results->records.front().value.outputs, operand, &liquid::action_output::path);
            if (found != results->records.front().value.outputs.end()) {
                return liquid::runtime::block_on(repository->abi(found->blob));
            }
        }
    }
    const auto path = std::filesystem::path{operand};
    const auto absolute = path.is_absolute() ? path : root / path;
    if (!std::filesystem::is_regular_file(absolute)) {
        if (!repository) {
            return std::unexpected(repository.error());
        }
        return std::unexpected(liquid::error{
            liquid::errc::not_found, "ABI input does not exist", std::string{operand}});
    }
    auto bytes = read_file(absolute);
    return bytes ? liquid::extract_abi(*bytes)
                 : liquid::result<liquid::abi_descriptor>{std::unexpected(bytes.error())};
}

void print_descriptor(const liquid::abi_descriptor& descriptor) {
    std::println("subject: {}", liquid::to_string(descriptor.subject));
    std::println("architecture: {}", descriptor.architecture);
    std::println("install-name: {}", descriptor.install_name.value_or("none"));
    if (descriptor.truncated) {
        std::println("truncated: symbol name or byte limit reached");
    }
    for (const auto& library : descriptor.needed_libraries) {
        std::println("needed: {}", library);
    }
    for (const auto& symbol : descriptor.exported_symbols) {
        std::println("symbol: {}{} {} size={} binding={} visibility={}{}",
                     symbol.name,
                     symbol.version ? "@" + *symbol.version : "",
                     kind_name(symbol.kind),
                     symbol.size,
                     symbol.binding,
                     symbol.visibility,
                     symbol.signature ? " signature=" + *symbol.signature : "");
    }
}

void print_report(const liquid::abi_report& report) {
    std::println("verdict: {}", verdict_name(report.verdict));
    for (const auto& change : report.changes) {
        std::println("change: {} {}: {}", change.symbol, change_name(change.kind), change.detail);
    }
}

} // namespace

auto abi(int argc, char** argv) -> int {
    if (argc < 2) {
        return usage("abi <show|diff> ...");
    }
    const auto operation = std::string_view{argv[0]};
    auto json = false;
    auto operands = std::vector<std::string_view>{};
    for (auto index = 1; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (argument.starts_with('-')) {
            return usage("abi accepts only --json");
        } else {
            operands.push_back(argument);
        }
    }
    if (operation == "show") {
        if (operands.size() != 1) {
            return usage("abi show <path|digest> [--json]");
        }
        auto descriptor = resolve(operands.front());
        if (!descriptor) {
            return fail(descriptor.error());
        }
        if (json) {
            auto encoded = liquid::abi_json(*descriptor);
            if (!encoded) {
                return fail(encoded.error());
            }
            std::println("{}", *encoded);
        } else {
            print_descriptor(*descriptor);
        }
        return 0;
    }
    if (operation == "diff") {
        if (operands.size() != 2) {
            return usage("abi diff <a> <b> [--json]");
        }
        auto old_descriptor = resolve(operands[0]);
        auto new_descriptor = resolve(operands[1]);
        if (!old_descriptor || !new_descriptor) {
            return fail(old_descriptor ? new_descriptor.error() : old_descriptor.error());
        }
        const auto report = liquid::diff_abi(*old_descriptor, *new_descriptor);
        if (json) {
            auto encoded = liquid::abi_json(report);
            if (!encoded) {
                return fail(encoded.error());
            }
            std::println("{}", *encoded);
        } else {
            print_report(report);
        }
        return 0;
    }
    return usage("abi <show|diff> ...");
}

} // namespace liquid_cli
