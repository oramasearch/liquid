#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <format>
#include <iostream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <cerrno>
#include <fcntl.h>
#include <termios.h>
#include <unistd.h>
#endif

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

auto json_string(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        switch (character) {
        case '\\':
            output += "\\\\";
            break;
        case '\"':
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

auto print_identity(const liquid::actor& actor, bool json) -> void {
    if (json) {
        std::println("{{\"id\":{},\"algorithm\":{},\"fingerprint\":{},\"key_fingerprint\":{},"
                     "\"custody\":{}}}",
                     json_string(actor.taap_id),
                     json_string(actor.algorithm),
                     json_string(actor.fingerprint),
                     json_string(actor.key_fingerprint),
                     json_string(actor.custody));
        return;
    }
    std::println("id={}", actor.taap_id);
    std::println("algorithm={}", actor.algorithm);
    std::println("fingerprint={}", actor.fingerprint);
    std::println("key_fingerprint={}", actor.key_fingerprint);
    std::println("custody={}", actor.custody);
}

auto warn_custody(const liquid::identity_custody_status& status) -> void {
    if (status.file_permissions_only) {
        std::println(stderr,
                     "warning: the identity signing key is protected only by file permissions; "
                     "run `liquid identity custody --to passphrase` (or keychain on macOS) to seal "
                     "it");
    } else if (status.clear_copy_present) {
        std::println(stderr,
                     "warning: a clear identity wrapping key is still on disk at {}; run `liquid "
                     "identity custody --to {}` to remove it",
                     (status.path.parent_path() / ".wrapping.key").string(),
                     status.kind);
    }
}

// The local identity with how its software key is protected: `wrapping_key_custody` is the
// custody URI of the key that seals the signing key ("file" means file permissions only).
auto print_local_identity(const liquid::actor& actor,
                          const liquid::identity_custody_status& status,
                          bool json) -> void {
    if (json) {
        std::println("{{\"id\":{},\"algorithm\":{},\"fingerprint\":{},\"key_fingerprint\":{},"
                     "\"custody\":{},\"wrapping_key_custody\":{},"
                     "\"file_permissions_only\":{}}}",
                     json_string(actor.taap_id),
                     json_string(actor.algorithm),
                     json_string(actor.fingerprint),
                     json_string(actor.key_fingerprint),
                     json_string(actor.custody),
                     json_string(status.custody),
                     status.file_permissions_only ? "true" : "false");
    } else {
        print_identity(actor, false);
        std::println("wrapping_key_custody={}", status.custody);
        std::println("file_permissions_only={}", status.file_permissions_only ? "true" : "false");
    }
    // A hardware key is not sealed by the wrapping key, so its custody is not weakened by it.
    if (actor.custody.empty() || actor.custody == "software") {
        warn_custody(status);
    }
}

auto print_custody(const liquid::identity_custody_status& status, bool json) -> void {
    if (json) {
        std::println("{{\"wrapping_key_custody\":{},\"kind\":{},\"file_permissions_only\":{},"
                     "\"clear_copy_present\":{},\"path\":{}}}",
                     json_string(status.custody),
                     json_string(status.kind),
                     status.file_permissions_only ? "true" : "false",
                     status.clear_copy_present ? "true" : "false",
                     json_string(status.path.string()));
    } else {
        std::println("wrapping_key_custody={}", status.custody);
        std::println("kind={}", status.kind);
        std::println("file_permissions_only={}", status.file_permissions_only ? "true" : "false");
        std::println("clear_copy_present={}", status.clear_copy_present ? "true" : "false");
        std::println("path={}", status.path.string());
    }
    warn_custody(status);
}

#if !defined(_WIN32)

auto write_terminal(int terminal, std::string_view text) -> void {
    while (!text.empty()) {
        const auto written = ::write(terminal, text.data(), text.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            return;
        }
        text.remove_prefix(static_cast<std::size_t>(written));
    }
}

// Reads one line from the terminal with echo off. Returns nothing at end of input.
auto read_hidden_line(int terminal, std::string_view prompt) -> std::optional<std::string> {
    write_terminal(terminal, prompt);
    auto saved = termios{};
    const auto restore = ::tcgetattr(terminal, &saved) == 0;
    if (restore) {
        auto hidden = saved;
        hidden.c_lflag &= ~static_cast<tcflag_t>(ECHO);
        static_cast<void>(::tcsetattr(terminal, TCSAFLUSH, &hidden));
    }
    auto line = std::string{};
    auto complete = false;
    for (;;) {
        auto character = char{};
        const auto count = ::read(terminal, &character, 1);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            break;
        }
        if (character == '\n') {
            complete = true;
            break;
        }
        line.push_back(character);
    }
    if (restore) {
        static_cast<void>(::tcsetattr(terminal, TCSAFLUSH, &saved));
    }
    write_terminal(terminal, "\n");
    if (!complete) {
        std::ranges::fill(line, '\0');
        return std::nullopt;
    }
    if (line.ends_with('\r')) {
        line.pop_back();
    }
    return line;
}

#endif

// Asks for the identity passphrase on the controlling terminal, never from an argument.
auto prompt_passphrase(bool confirm) -> liquid::result<std::string> {
    const auto unavailable =
        liquid::error{liquid::errc::unauthorized,
                      "the identity key needs a passphrase and no terminal is "
                      "available: set LIQUID_IDENTITY_PASSPHRASE_FILE, or "
                      "choose another custody with LIQUID_IDENTITY_KEY_CUSTODY",
                      "identity passphrase"};
#if defined(_WIN32)
    static_cast<void>(confirm);
    return std::unexpected(unavailable);
#else
    if (::isatty(STDIN_FILENO) == 0) {
        return std::unexpected(unavailable);
    }
    const auto terminal = ::open("/dev/tty", O_RDWR | O_CLOEXEC);
    if (terminal < 0) {
        return std::unexpected(unavailable);
    }
    auto first = read_hidden_line(
        terminal, confirm ? "New liquid identity passphrase: " : "liquid identity passphrase: ");
    if (!first || !confirm) {
        static_cast<void>(::close(terminal));
        if (!first) {
            return std::unexpected(unavailable);
        }
        return std::move(*first);
    }
    auto second = read_hidden_line(terminal, "Repeat the passphrase: ");
    static_cast<void>(::close(terminal));
    if (!second || *second != *first) {
        std::ranges::fill(*first, '\0');
        if (second) {
            std::ranges::fill(*second, '\0');
        }
        return std::unexpected(liquid::error{
            liquid::errc::invalid_argument, "the passphrases do not match", "identity passphrase"});
    }
    std::ranges::fill(*second, '\0');
    return std::move(*first);
#endif
}

auto base64url(std::span<const std::byte> bytes) -> std::string {
    constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"};
    auto output = std::string{};
    output.reserve((bytes.size() * 4 + 2) / 3);
    auto pending = std::uint32_t{};
    auto bits = 0;
    for (const auto byte : bytes) {
        pending = (pending << 8) | std::to_integer<std::uint8_t>(byte);
        bits += 8;
        while (bits >= 6) {
            bits -= 6;
            output += alphabet[(pending >> bits) & 63U];
        }
    }
    if (bits != 0) {
        output += alphabet[(pending << (6 - bits)) & 63U];
    }
    return output;
}

auto print_document(const liquid::signer& signer) -> int {
    const auto document = signer.document();
    if (!document) {
        return fail(document.error());
    }
    const auto encoded = liquid::encode_identity_document(*document);
    if (!encoded) {
        return fail(encoded.error());
    }
    std::print("{}", base64url(*encoded));
    return 0;
}

auto print_chain(const liquid::signer& signer) -> int {
    const auto chain = liquid::load_identity_chain(signer.identity().taap_id);
    if (!chain) {
        return fail(chain.error());
    }
    const auto encoded = liquid::encode_identity_chain(*chain);
    if (!encoded) {
        return fail(encoded.error());
    }
    std::print("{}", base64url(*encoded));
    return 0;
}

auto base64url_value(char character) -> std::optional<std::uint32_t> {
    if (character >= 'A' && character <= 'Z') {
        return static_cast<std::uint32_t>(character - 'A');
    }
    if (character >= 'a' && character <= 'z') {
        return static_cast<std::uint32_t>(character - 'a' + 26);
    }
    if (character >= '0' && character <= '9') {
        return static_cast<std::uint32_t>(character - '0' + 52);
    }
    if (character == '-') {
        return 62U;
    }
    if (character == '_') {
        return 63U;
    }
    return std::nullopt;
}

auto decode_base64url(std::string_view text) -> std::optional<std::vector<std::byte>> {
    auto output = std::vector<std::byte>{};
    output.reserve(text.size() * 3 / 4);
    auto pending = std::uint32_t{};
    auto bits = 0;
    for (const auto character : text) {
        const auto value = base64url_value(character);
        if (!value) {
            return std::nullopt;
        }
        pending = (pending << 6U) | *value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(std::byte{static_cast<std::uint8_t>((pending >> bits) & 0xffU)});
        }
    }
    if (bits >= 6 || (pending & ((1U << bits) - 1U)) != 0) {
        return std::nullopt;
    }
    return output;
}

auto trimmed(std::string_view text) -> std::string_view {
    const auto first = text.find_first_not_of(" \t\r\n");
    if (first == std::string_view::npos) {
        return {};
    }
    const auto last = text.find_last_not_of(" \t\r\n");
    return text.substr(first, last - first + 1);
}

auto trust(std::string_view source, bool accept_compromise_rotation, bool json) -> int {
    auto text = std::string{source};
    if (source == "-") {
        text.assign(std::istreambuf_iterator<char>{std::cin}, std::istreambuf_iterator<char>{});
    }
    const auto bytes = decode_base64url(trimmed(text));
    if (!bytes) {
        return fail({liquid::errc::invalid_argument,
                     "identity chain is not unpadded base64url",
                     "identity trust"});
    }
    const auto chain = liquid::decode_identity_chain(*bytes);
    if (!chain) {
        return fail(chain.error());
    }
    const auto trusted = liquid::trust_identity_chain(
        *chain, {.accept_compromise_rotation = accept_compromise_rotation});
    if (!trusted) {
        return fail(trusted.error());
    }
    print_identity(trusted->subject, json);
    return 0;
}

} // namespace

auto install_identity_custody() -> void {
    liquid::set_identity_custody_options({.custody = {}, .passphrase = prompt_passphrase});
}

auto identity(int argc, char** argv) -> int {
    constexpr auto synopsis = std::string_view{
        "identity [show [--document|--chain]|rotate [--to software|fido2|piv:9c]|custody [--to "
        "passphrase|keychain|file|<custody-uri>]|trust <chain|-> [--accept-compromise-rotation]] "
        "[--json]"};
    auto command = std::string_view{"show"};
    auto json = false;
    auto document = false;
    auto chain = false;
    auto accept_compromise_rotation = false;
    auto custody = std::string{"software"};
    auto custody_given = false;
    auto source = std::optional<std::string_view>{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (argument == "--document") {
            document = true;
        } else if (argument == "--chain") {
            chain = true;
        } else if (argument == "--accept-compromise-rotation") {
            accept_compromise_rotation = true;
        } else if (argument == "--to" && index + 1 < argc) {
            custody = argv[++index];
            custody_given = true;
        } else if (argument == "show" || argument == "rotate" || argument == "custody" ||
                   argument == "trust") {
            command = argument;
        } else if (command == "trust" && !source) {
            source = argument;
        } else {
            return usage(synopsis);
        }
    }
    if (command != "rotate" && command != "custody" && custody_given) {
        return usage("only identity rotate and identity custody accept --to");
    }
    if ((document || chain) && command != "show") {
        return usage("--document and --chain are only supported by identity show");
    }
    if (document && chain) {
        return usage("identity show accepts one of --document and --chain");
    }
    if ((document || chain) && json) {
        return usage("identity show --document and --chain cannot be combined with --json");
    }
    if (accept_compromise_rotation && command != "trust") {
        return usage("--accept-compromise-rotation is only supported by identity trust");
    }
    if (command == "trust") {
        if (!source) {
            return usage("identity trust requires a chain or - for standard input");
        }
        return trust(*source, accept_compromise_rotation, json);
    }
    if (command == "custody") {
        if (custody_given) {
            auto migrated =
                liquid::runtime::block_on(liquid::migrate_local_identity_custody(custody));
            if (!migrated) {
                return fail(migrated.error());
            }
            print_custody(*migrated, json);
            return 0;
        }
        const auto status = liquid::local_identity_custody();
        if (!status) {
            return fail(status.error());
        }
        print_custody(*status, json);
        return 0;
    }
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return fail(signer.error());
    }
    if (command == "rotate") {
        auto rotated = liquid::runtime::block_on(signer->rotate_to(std::move(custody)));
        if (!rotated) {
            return fail(rotated.error());
        }
    }
    if (document) {
        return print_document(*signer);
    }
    if (chain) {
        return print_chain(*signer);
    }
    const auto status = liquid::local_identity_custody();
    if (!status) {
        return fail(status.error());
    }
    print_local_identity(signer->identity(), *status, json);
    return 0;
}

} // namespace liquid_cli
