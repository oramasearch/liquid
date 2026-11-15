// SPDX-License-Identifier: MIT

#include <cstddef>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/transparency.hpp"

namespace liquid_cli {

auto repo_log(int argc, char** argv) -> int;

namespace {

auto fail(const liquid::error& value, int status = 1) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return status;
}

auto usage() -> int {
    std::println(
        stderr,
        "error: usage: log [--ops] [--json] | log verify <log-url> <attestation-base64url> | "
        "log consistency <log-url> | log scan <log-url> <identity>");
    return 2;
}

auto base64url_decode(std::string_view value) -> liquid::result<std::vector<std::byte>> {
    if (value.empty() || value.size() % 4 == 1) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, "attestation is not base64url", {}});
    }
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    auto output = std::vector<std::byte>{};
    auto accumulator = std::uint32_t{};
    auto bits = 0U;
    for (const auto character : value) {
        const auto position = alphabet.find(character);
        if (position == std::string_view::npos) {
            return std::unexpected(
                liquid::error{liquid::errc::invalid_argument, "attestation is not base64url", {}});
        }
        accumulator = (accumulator << 6U) | static_cast<std::uint32_t>(position);
        bits += 6;
        if (bits < 8) {
            continue;
        }
        bits -= 8;
        output.push_back(std::byte{static_cast<unsigned char>((accumulator >> bits) & 0xffU)});
    }
    return output;
}

auto open_repository() -> liquid::result<liquid::repository> {
    return liquid::runtime::block_on(liquid::repository::open(std::filesystem::current_path()));
}

auto print_checkpoint(const liquid::transparency::checkpoint_trust& value) -> void {
    std::println("checkpoint_sequence={} tree_size={} bootstrap_warning={}",
                 value.value.sequence,
                 value.value.tree_size,
                 value.bootstrap_warning);
}

} // namespace

auto transparency_log(int argc, char** argv) -> int {
    if (argc >= 1 && std::string_view{argv[0]} == "timestamps") {
        return repo_log(argc, argv);
    }
    if (argc == 2 && std::string_view{argv[0]} == "verify" &&
        std::string_view{argv[1]} == "--timestamps") {
        return repo_log(argc, argv);
    }
    if (argc == 0 ||
        (argc == 1 &&
         (std::string_view{argv[0]} == "--json" || std::string_view{argv[0]} == "--ops")) ||
        (argc == 2 &&
         ((std::string_view{argv[0]} == "--ops" && std::string_view{argv[1]} == "--json") ||
          (std::string_view{argv[0]} == "--json" && std::string_view{argv[1]} == "--ops")))) {
        return repo_log(argc, argv);
    }
    const auto operation = std::string_view{argv[0]};
    if (operation == "verify" && argc == 3) {
        auto source = liquid::transparency::http_log_source::create(argv[1]);
        if (!source) {
            return fail(source.error(), 2);
        }
        auto encoded = base64url_decode(argv[2]);
        if (!encoded) {
            return fail(encoded.error());
        }
        auto repository = open_repository();
        if (!repository) {
            return fail(repository.error());
        }
        auto client = liquid::transparency::log_client{*source};
        auto verified =
            liquid::runtime::block_on(client.verify_inclusion(*repository, std::move(*encoded)));
        if (!verified) {
            return fail(verified.error());
        }
        print_checkpoint(*verified);
        return 0;
    }
    if (operation == "consistency" && argc == 2) {
        auto source = liquid::transparency::http_log_source::create(argv[1]);
        if (!source) {
            return fail(source.error(), 2);
        }
        auto repository = open_repository();
        if (!repository) {
            return fail(repository.error());
        }
        auto client = liquid::transparency::log_client{*source};
        auto verified = liquid::runtime::block_on(client.verify_consistency(*repository));
        if (!verified) {
            return fail(verified.error());
        }
        print_checkpoint(*verified);
        return 0;
    }
    if (operation == "scan" && argc == 3) {
        auto source = liquid::transparency::http_log_source::create(argv[1]);
        if (!source) {
            return fail(source.error(), 2);
        }
        auto client = liquid::transparency::log_client{*source};
        auto scan = liquid::runtime::block_on(client.scan(argv[2]));
        if (!scan) {
            return fail(scan.error());
        }
        for (const auto& match : scan->matches) {
            std::println("index={} profile={} issuer={} subject={}",
                         match.index,
                         match.profile,
                         match.issuer,
                         match.subject);
        }
        return 0;
    }
    return usage();
}

} // namespace liquid_cli
