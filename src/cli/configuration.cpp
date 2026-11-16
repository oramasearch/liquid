// SPDX-License-Identifier: MIT

#include <algorithm>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <optional>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/configuration.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

#if defined(_WIN32)
#include <fcntl.h>
#include <io.h>
#endif

namespace liquid_cli {
namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto read_text(const std::filesystem::path& path) -> liquid::result<std::string> {
    auto stream = std::ifstream{path, std::ios::binary};
    if (!stream) {
        return std::unexpected(
            liquid::error{liquid::errc::io, "cannot read configuration", path.generic_string()});
    }
    return std::string{std::istreambuf_iterator<char>{stream}, {}};
}

auto load_document(const std::filesystem::path& path)
    -> liquid::result<liquid::configuration_document> {
    auto text = read_text(path);
    if (!text) {
        return std::unexpected(text.error());
    }
    return liquid::parse_configuration(*text, path);
}

auto configuration_paths(const std::filesystem::path& selected)
    -> liquid::result<std::vector<std::filesystem::path>> {
    if (std::filesystem::is_regular_file(selected)) {
        if (selected.extension() != ".liquid") {
            return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                                 "configuration files use .liquid",
                                                 selected.generic_string()});
        }
        return std::vector<std::filesystem::path>{selected};
    }
    if (!std::filesystem::is_directory(selected)) {
        return std::unexpected(liquid::error{liquid::errc::not_found,
                                             "configuration path does not exist",
                                             selected.generic_string()});
    }
    auto output = std::vector<std::filesystem::path>{};
    auto code = std::error_code{};
    for (auto iterator =
             std::filesystem::recursive_directory_iterator{
                 selected, std::filesystem::directory_options::skip_permission_denied, code};
         !code && iterator != std::filesystem::recursive_directory_iterator{};
         iterator.increment(code)) {
        if (iterator->is_directory() &&
            (iterator->path().filename() == ".git" ||
             iterator->path().filename().string().starts_with("build"))) {
            iterator.disable_recursion_pending();
            continue;
        }
        if (iterator->is_regular_file() && iterator->path().extension() == ".liquid") {
            output.push_back(iterator->path());
        }
    }
    if (code) {
        return std::unexpected(
            liquid::error{liquid::errc::io, code.message(), selected.generic_string()});
    }
    std::ranges::sort(output);
    return output;
}

auto json_escape(std::string_view input) -> std::string {
    auto output = std::string{};
    for (const auto value : input) {
        switch (value) {
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
            output.push_back(value);
            break;
        }
    }
    return output;
}

auto origin_json(const liquid::configuration_record_origin& origin) -> std::string {
    auto output = std::string{"{\"kind\":\""};
    output += origin.kind == liquid::configuration_origin_kind::generated ? "generated" : "source";
    output += "\",\"file\":\"" + json_escape(origin.file.generic_string()) + "\"";
    if (origin.result) {
        output += ",\"result\":\"" + liquid::to_string(*origin.result) + "\"";
    }
    return output + "}";
}

auto lower_json(const liquid::lowered_configuration& value) -> std::string {
    auto output = std::string{"{\"v\":1,\"packages\":["};
    for (std::size_t index = 0; index < value.packages.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += "{\"name\":\"" + json_escape(value.packages[index].name) +
                  "\",\"origin\":" + origin_json(value.package_origins[index]) + "}";
    }
    output += "],\"targets\":[";
    for (std::size_t index = 0; index < value.targets.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += "{\"name\":\"" + json_escape(value.targets[index].name) +
                  "\",\"origin\":" + origin_json(value.targets[index].origin) + "}";
    }
    output += "],\"sources\":[";
    for (std::size_t index = 0; index < value.sources.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += "{\"name\":\"" + json_escape(value.sources[index].name) + "\",\"digest\":\"" +
                  liquid::to_string(value.sources[index].content) +
                  "\",\"origin\":" + origin_json(value.sources[index].origin) + "}";
    }
    output += "],\"locks\":[";
    for (std::size_t index = 0; index < value.locks.rules.size(); ++index) {
        if (index != 0) {
            output += ',';
        }
        output += "{\"name\":\"" + json_escape(value.locks.rules[index].name) + "\",\"paths\":[";
        for (std::size_t path_index = 0; path_index < value.locks.rules[index].paths.size();
             ++path_index) {
            if (path_index != 0) {
                output += ',';
            }
            output += "\"" + json_escape(value.locks.rules[index].paths[path_index]) + "\"";
        }
        output += "]}";
    }
    return output + "]}";
}

auto write_package_record(const liquid::lowered_configuration& lowered,
                          const std::filesystem::path& output,
                          const std::optional<std::string>& name) -> liquid::result<void> {
    const auto package =
        name ? std::ranges::find(lowered.packages, *name, &liquid::package_record::name)
             : lowered.packages.begin();
    if (package == lowered.packages.end() || (!name && lowered.packages.size() != 1)) {
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument,
            name ? "no package named '" + *name + "' in configuration"
                 : "--output-record requires exactly one package; select one with --package <name>",
            output.generic_string()});
    }
    auto encoded = liquid::encode_package(*package);
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    if (output == "-") {
#if defined(_WIN32)
        if (_setmode(_fileno(stdout), _O_BINARY) == -1) {
            return std::unexpected(
                liquid::error{liquid::errc::io, "cannot set binary stdout", "stdout"});
        }
#endif
        if (std::fwrite(encoded->data(), 1, encoded->size(), stdout) != encoded->size() ||
            std::fflush(stdout) != 0) {
            return std::unexpected(
                liquid::error{liquid::errc::io, "cannot write package record", "stdout"});
        }
        return {};
    }
    auto stream = std::ofstream{output, std::ios::binary | std::ios::trunc};
    stream.write(reinterpret_cast<const char*>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
    stream.close();
    if (!stream) {
        return std::unexpected(liquid::error{
            liquid::errc::io, "cannot write package record", output.generic_string()});
    }
    return {};
}

auto lower_command(const std::filesystem::path& path,
                   bool json,
                   bool trace,
                   const std::optional<std::filesystem::path>& output,
                   const std::optional<std::string>& package) -> liquid::runtime::task<int> {
    auto document = load_document(path);
    if (!document) {
        co_return std::unexpected(document.error());
    }
    auto lowered = liquid::lower(*document);
    if (!lowered) {
        co_return std::unexpected(lowered.error());
    }
    if (!lowered->generators.empty()) {
        auto repository = co_await liquid::repository::open(std::filesystem::current_path());
        if (!repository) {
            co_return std::unexpected(repository.error());
        }
        lowered = co_await liquid::run_generators(*repository, std::move(*document));
        if (!lowered) {
            co_return std::unexpected(lowered.error());
        }
    }
    if (output) {
        auto written = write_package_record(*lowered, *output, package);
        if (!written) {
            co_return std::unexpected(written.error());
        }
        co_return 0;
    }
    if (json) {
        std::println("{}", lower_json(*lowered));
        co_return 0;
    }
    for (std::size_t index = 0; index < lowered->packages.size(); ++index) {
        std::print("package {}", lowered->packages[index].name);
        if (trace) {
            std::print(" origin={}", origin_json(lowered->package_origins[index]));
        }
        std::println();
    }
    for (const auto& target : lowered->targets) {
        std::print("target {}", target.name);
        if (trace) {
            std::print(" origin={}", origin_json(target.origin));
        }
        std::println();
    }
    for (const auto& source : lowered->sources) {
        std::print("source {} {}", source.name, liquid::to_string(source.content));
        if (trace) {
            std::print(" origin={}", origin_json(source.origin));
        }
        std::println();
    }
    for (const auto& rule : lowered->locks.rules) {
        std::println("lock {}", rule.name);
    }
    co_return 0;
}

} // namespace

auto config(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("config <check|lower|format> ...");
    }
    const auto operation = std::string_view{argv[0]};
    if (operation == "check") {
        if (argc != 2) {
            return usage("config check <file|dir>");
        }
        auto paths = configuration_paths(argv[1]);
        if (!paths) {
            return fail(paths.error());
        }
        auto failed = false;
        for (const auto& path : *paths) {
            auto document = load_document(path);
            if (!document) {
                std::println(stderr, "{}", document.error().message);
                failed = true;
                continue;
            }
            for (const auto& diagnostic : liquid::check(*document)) {
                std::println(stderr, "{}", liquid::to_string(diagnostic));
                failed = true;
            }
        }
        return failed ? 1 : 0;
    }
    if (operation == "lower") {
        if (argc < 2) {
            return usage("config lower <file> [--json] [--trace] [--output-record <file|-> "
                         "[--package <name>]]");
        }
        auto json = false;
        auto trace = false;
        auto output = std::optional<std::filesystem::path>{};
        auto package = std::optional<std::string>{};
        for (auto index = 2; index < argc; ++index) {
            if (std::string_view{argv[index]} == "--json") {
                json = true;
            } else if (std::string_view{argv[index]} == "--trace") {
                trace = true;
            } else if (std::string_view{argv[index]} == "--output-record") {
                if (output || ++index == argc || std::string_view{argv[index]}.empty()) {
                    return usage("--output-record requires one file or - for stdout");
                }
                output = argv[index];
            } else if (std::string_view{argv[index]} == "--package") {
                if (package || ++index == argc || std::string_view{argv[index]}.empty()) {
                    return usage("--package requires one package name");
                }
                package = argv[index];
            } else {
                return usage("config lower <file> [--json] [--trace] [--output-record <file|-> "
                             "[--package <name>]]");
            }
        }
        if (output && (json || trace)) {
            return usage("--output-record cannot be combined with --json or --trace");
        }
        if (package && !output) {
            return usage("--package requires --output-record");
        }
        auto lowered =
            liquid::runtime::block_on(lower_command(argv[1], json, trace, output, package));
        return lowered ? *lowered : fail(lowered.error());
    }
    if (operation == "format") {
        if (argc != 2) {
            return usage("config format <file>");
        }
        const auto path = std::filesystem::path{argv[1]};
        auto document = load_document(path);
        if (!document) {
            return fail(document.error());
        }
        auto formatted = liquid::format_configuration(*document);
        if (!formatted) {
            return fail(formatted.error());
        }
        auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return fail({liquid::errc::io, "cannot write configuration", path.generic_string()});
        }
        stream << *formatted;
        if (!stream) {
            return fail({liquid::errc::io, "cannot write configuration", path.generic_string()});
        }
        return 0;
    }
    return usage("config <check|lower|format> ...");
}

auto configuration_build(std::string target_name, bool json) -> int {
    const auto root = std::filesystem::current_path();
    auto path = root / "liquid.liquid";
    if (!std::filesystem::is_regular_file(path)) {
        auto paths = configuration_paths(root);
        if (!paths) {
            return fail(paths.error());
        }
        const auto top = std::ranges::find_if(
            *paths, [&](const auto& candidate) { return candidate.parent_path() == root; });
        if (top == paths->end()) {
            return fail({liquid::errc::not_found,
                         "repository has no top-level .liquid document",
                         root.generic_string()});
        }
        path = *top;
    }
    auto document = load_document(path);
    if (!document) {
        return fail(document.error());
    }
    auto repository = liquid::runtime::block_on(liquid::repository::open(root));
    if (!repository) {
        return fail(repository.error());
    }
    auto lowered = liquid::runtime::block_on(liquid::run_generators(*repository, *document));
    if (!lowered) {
        return fail(lowered.error());
    }
    auto built =
        liquid::runtime::block_on(liquid::build_configuration(*repository, *lowered, target_name));
    if (!built) {
        return fail(built.error());
    }
    if (json) {
        std::println("{{\"v\":1,\"target\":\"{}\",\"result\":\"{}\"}}",
                     json_escape(target_name),
                     liquid::to_string(built->value));
    } else {
        std::println("result={}", liquid::to_string(built->value));
    }
    return 0;
}

} // namespace liquid_cli
