// SPDX-License-Identifier: MIT

#include <cstdlib>
#include <filesystem>
#include <format>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/credential.hpp"
#include "liquid/drive.hpp"
#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr, "error: usage: clone <url|drive-folder> [<dir>] [--json] ()");
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

auto source_is_url(std::string_view source) -> bool {
    return source.find("://") != std::string_view::npos;
}

auto remote_from_drive_folder(const std::filesystem::path& folder) -> liquid::result<std::string> {
    auto code = std::error_code{};
    if (!std::filesystem::is_directory(folder, code) || code ||
        !std::filesystem::exists(folder / "LIQUID_REPO", code) || code) {
        return std::unexpected(
            liquid::error{liquid::errc::not_found, "drive repository not found", folder.string()});
    }
    auto client = liquid::runtime::block_on(liquid::drive_client::open(folder));
    if (!client) {
        return std::unexpected(client.error());
    }
    auto status = liquid::runtime::block_on(client->status());
    if (!status) {
        return std::unexpected(status.error());
    }
    return std::move(status->remote);
}

auto default_directory(std::string_view remote) -> std::filesystem::path {
    auto value = remote;
    const auto suffix = value.find_first_of("?#");
    value = value.substr(0, suffix);
    while (!value.empty() && value.back() == '/') {
        value.remove_suffix(1);
    }
    const auto scheme = value.find("://");
    const auto path_start =
        scheme == std::string_view::npos ? std::string_view::npos : value.find('/', scheme + 3);
    if (path_start == std::string_view::npos) {
        return "repository";
    }
    const auto name_start = value.find_last_of('/') + 1;
    auto name = std::string{value.substr(name_start)};
    if (name.ends_with(".git")) {
        name.resize(name.size() - 4);
    }
    return name.empty() ? std::filesystem::path{"repository"} : std::filesystem::path{name};
}

auto validate_target(const std::filesystem::path& target) -> liquid::result<bool> {
    auto code = std::error_code{};
    const auto exists = std::filesystem::exists(target, code);
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), target.string()});
    }
    if (!exists) {
        return false;
    }
    if (std::filesystem::is_symlink(target, code)) {
        return std::unexpected(liquid::error{liquid::errc::already_exists,
                                             "clone target must not be a symbolic link",
                                             target.string()});
    }
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), target.string()});
    }
    if (!std::filesystem::is_directory(target, code)) {
        return std::unexpected(liquid::error{
            liquid::errc::already_exists, "clone target is not a directory", target.string()});
    }
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), target.string()});
    }
    if (!std::filesystem::is_empty(target, code)) {
        return std::unexpected(liquid::error{
            liquid::errc::already_exists, "clone target is not empty", target.string()});
    }
    if (code) {
        return std::unexpected(liquid::error{liquid::errc::io, code.message(), target.string()});
    }
    return true;
}

auto clean_failed_target(const std::filesystem::path& target, bool existed) -> void {
    auto code = std::error_code{};
    if (!std::filesystem::is_directory(target, code) || code) {
        return;
    }
    auto entries = std::vector<std::filesystem::path>{};
    for (auto entry = std::filesystem::directory_iterator{target, code};
         !code && entry != std::filesystem::directory_iterator{};
         entry.increment(code)) {
        entries.push_back(entry->path());
    }
    for (const auto& entry : entries) {
        std::filesystem::remove_all(entry, code);
        if (code) {
            return;
        }
    }
    if (!existed && !code) {
        std::filesystem::remove(target, code);
    }
}

} // namespace

auto clone(int argc, char** argv) -> int {
    auto positional = std::vector<std::string_view>{};
    auto json = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            if (json) {
                return usage();
            }
            json = true;
            continue;
        }
        positional.push_back(argument);
    }
    if (positional.empty() || positional.size() > 2) {
        return usage();
    }

    auto remote = std::string{positional.front()};
    if (!source_is_url(positional.front())) {
        auto configured = remote_from_drive_folder(std::filesystem::path{positional.front()});
        if (!configured) {
            return fail(configured.error());
        }
        remote = std::move(*configured);
    }
    const auto target =
        positional.size() == 2 ? std::filesystem::path{positional[1]} : default_directory(remote);
    auto valid_target = validate_target(target);
    if (!valid_target) {
        return fail(valid_target.error());
    }

    auto options = liquid::drive_options{};
    if (const auto token = liquid::lake_token(); !token.empty()) {
        options.bearer_token = token;
    }
    auto client =
        liquid::runtime::block_on(liquid::drive_client::open(target, remote, std::move(options)));
    if (!client) {
        clean_failed_target(target, *valid_target);
        return fail(client.error());
    }
    auto pinned = liquid::runtime::block_on(client->pin(""));
    if (!pinned) {
        client = liquid::drive_client{};
        clean_failed_target(target, *valid_target);
        return fail(pinned.error());
    }
    auto report = liquid::runtime::block_on(client->sync());
    if (!report) {
        client = liquid::drive_client{};
        clean_failed_target(target, *valid_target);
        return fail(report.error());
    }
    for (const auto& reference : report->status.references) {
        if (reference.outcome == liquid::sync_ref_outcome_kind::refused) {
            client = liquid::drive_client{};
            clean_failed_target(target, *valid_target);
            return fail(
                liquid::error{liquid::errc::unauthorized, reference.reason, reference.name});
        }
    }

    auto code = std::error_code{};
    auto landed = std::filesystem::weakly_canonical(std::filesystem::absolute(target), code);
    if (code) {
        landed = target;
    }
    if (json) {
        std::println("{{\"path\":{},\"remote\":{},\"objects_received\":{},"
                     "\"bytes_received\":{}}}",
                     json_string(landed.string()),
                     json_string(remote),
                     report->received.objects,
                     report->received.bytes);
    } else {
        std::println("cloned {} to {}", remote, landed.string());
    }
    return 0;
}

} // namespace liquid_cli
