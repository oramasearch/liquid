// SPDX-License-Identifier: MIT

#include <charconv>
#include <cstdint>
#include <filesystem>
#include <print>
#include <string>
#include <string_view>

#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {} ()", message);
    return 2;
}

auto parse_count(std::string_view text, std::string_view name) -> liquid::result<std::uint8_t> {
    auto value = unsigned{};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        value == 0 || value > 255) {
        return std::unexpected(
            liquid::error{liquid::errc::invalid_argument, "invalid count", std::string{name}});
    }
    return static_cast<std::uint8_t>(value);
}

auto open_keys() -> liquid::result<std::shared_ptr<liquid::keyring>> {
    return liquid::runtime::block_on(
        liquid::keyring::open(std::filesystem::current_path() / ".liquid-store" / "keys"));
}

auto show(int argc, char** argv) -> int {
    if (argc > 1)
        return usage("keys show [key-id]");
    auto keys = open_keys();
    if (!keys)
        return fail(keys.error());
    auto records = liquid::runtime::block_on((*keys)->show(argc == 1 ? argv[0] : ""));
    if (!records)
        return fail(records.error());
    for (const auto& record : *records) {
        std::println("{}@{} custody={} current={} revoked={}",
                     record.key_id,
                     record.version,
                     record.custody,
                     record.current,
                     record.revoked);
    }
    return 0;
}

auto rewrap(int argc, char** argv) -> int {
    if (argc != 3 || std::string_view{argv[1]} != "--to")
        return usage("keys rewrap <key-id> --to <custody-uri>");
    auto keys = open_keys();
    if (!keys)
        return fail(keys.error());
    auto provider = liquid::runtime::block_on(liquid::key_provider_from_uri(argv[2]));
    if (!provider)
        return fail(provider.error());
    auto changed = liquid::runtime::block_on((*keys)->rewrap(argv[0], std::move(*provider)));
    if (!changed)
        return fail(changed.error());
    std::println("rewrapped {}", argv[0]);
    return 0;
}

auto rotate(int argc, char** argv) -> int {
    if (argc != 1)
        return usage("keys rotate <key-id>");
    auto keys = open_keys();
    if (!keys)
        return fail(keys.error());
    auto report = liquid::runtime::block_on((*keys)->rotate(argv[0]));
    if (!report)
        return fail(report.error());
    std::println("rotated {} from version {} to {}",
                 report->key_id,
                 report->previous_version,
                 report->new_version);
    for (const auto& statement : report->limits)
        std::println("{}", statement);
    return 0;
}

auto revoke(int argc, char** argv) -> int {
    if (argc != 2)
        return usage("keys revoke <key-id> <version>");
    auto version = std::uint64_t{};
    const auto text = std::string_view{argv[1]};
    const auto parsed = std::from_chars(text.data(), text.data() + text.size(), version);
    if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() ||
        version == 0)
        return usage("keys revoke <key-id> <version>");
    auto keys = open_keys();
    if (!keys)
        return fail(keys.error());
    auto changed = liquid::runtime::block_on((*keys)->revoke(argv[0], version));
    if (!changed)
        return fail(changed.error());
    std::println("revoked {}@{}", argv[0], version);
    return 0;
}

auto split(int argc, char** argv) -> int {
    if (argc != 5 || std::string_view{argv[1]} != "--threshold" ||
        std::string_view{argv[3]} != "--shares")
        return usage("keys split <key-id> --threshold <k> --shares <n>");
    auto threshold = parse_count(argv[2], "threshold");
    auto count = parse_count(argv[4], "shares");
    if (!threshold)
        return fail(threshold.error());
    if (!count)
        return fail(count.error());
    auto custody = liquid::split_custody(*threshold, *count);
    if (!custody)
        return fail(custody.error());
    auto keys = open_keys();
    if (!keys)
        return fail(keys.error());
    auto changed =
        liquid::runtime::block_on((*keys)->rewrap(argv[0], std::move(custody->provider)));
    if (!changed)
        return fail(changed.error());
    for (const auto& share : custody->shares)
        std::println("{}", share);
    return 0;
}

} // namespace

auto keys(int argc, char** argv) -> int {
    if (argc < 1)
        return usage("keys <show|rewrap|rotate|revoke|split> ...");
    const auto command = std::string_view{argv[0]};
    if (command == "show")
        return show(argc - 1, argv + 1);
    if (command == "rewrap")
        return rewrap(argc - 1, argv + 1);
    if (command == "rotate")
        return rotate(argc - 1, argv + 1);
    if (command == "revoke")
        return revoke(argc - 1, argv + 1);
    if (command == "split")
        return split(argc - 1, argv + 1);
    return usage("keys <show|rewrap|rotate|revoke|split> ...");
}

} // namespace liquid_cli
