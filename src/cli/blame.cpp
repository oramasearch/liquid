// SPDX-License-Identifier: MIT

#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>

#include "liquid/daemon.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"

#include "cli/signer.hpp"

namespace liquid_cli {

[[nodiscard]] auto ensure_daemon(const std::filesystem::path& checkout) -> liquid::result<void>;

namespace {

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr, "error: usage: liquid blame <path> [--json]");
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

void print_json(std::string_view path,
                const liquid::change_id& change,
                std::span<const liquid::line_attribution> lines) {
    std::print("{{\"v\":1,\"path\":{},\"change\":{},\"lines\":[",
               json_string(path),
               json_string(liquid::to_string(change.value)));
    for (auto index = std::size_t{}; index < lines.size(); ++index) {
        const auto& line = lines[index];
        if (index != 0) {
            std::print(",");
        }
        std::print("{{\"line\":{},\"change\":{},\"session\":{},\"actor\":{},"
                   "\"identity_verified\":{},\"agent\":{},\"text\":{}}}",
                   line.line,
                   json_string(liquid::to_string(line.change.value)),
                   json_string(liquid::to_string(line.session)),
                   json_string(shown_signer(line.by, line.identity_verified)),
                   line.identity_verified,
                   line.agent,
                   json_string(line.text));
    }
    std::println("]}}");
}

} // namespace

auto blame(int argc, char** argv) -> int {
    auto path = std::string{};
    auto json = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (path.empty()) {
            path = argument;
        } else {
            return usage();
        }
    }
    if (path.empty()) {
        return usage();
    }
    const auto checkout = std::filesystem::current_path();
    auto source = liquid::runtime::block_on(liquid::repository::open(checkout));
    if (!source) {
        return fail(source.error());
    }
    auto change = liquid::runtime::block_on(source->head());
    if (!change) {
        return fail(change.error());
    }
    auto ready = ensure_daemon(checkout);
    if (!ready) {
        return fail(ready.error());
    }
    auto attached = liquid::runtime::block_on(liquid::daemon::attach(checkout));
    if (!attached) {
        return fail(attached.error());
    }
    auto lines = liquid::runtime::block_on(attached->attribution(path, *change));
    if (!lines) {
        return fail(lines.error());
    }
    if (json) {
        print_json(path, *change, *lines);
        return 0;
    }
    for (const auto& line : *lines) {
        std::println("{} change={} session={} actor={} agent={}\t{}",
                     line.line,
                     liquid::to_string(line.change.value),
                     liquid::to_string(line.session),
                     shown_signer(line.by, line.identity_verified),
                     line.agent,
                     line.text);
    }
    return 0;
}

} // namespace liquid_cli
