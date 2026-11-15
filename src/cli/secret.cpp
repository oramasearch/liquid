#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <optional>
#include <print>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "liquid/runtime.hpp"
#include "liquid/secret.hpp"

namespace liquid_cli {
namespace {

auto cli_error(liquid::errc code, std::string message, std::string context = {}) -> liquid::error {
    return {code, std::move(message), std::move(context)};
}

auto fail(const liquid::error& value) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(value.code), value.message, value.context);
    return 1;
}

auto usage(std::string_view message) -> int {
    std::println(stderr, "error: usage: {}", message);
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
                constexpr auto digits = std::string_view{"0123456789abcdef"};
                output += "\\u00";
                output += digits[static_cast<unsigned char>(character) >> 4U];
                output += digits[static_cast<unsigned char>(character) & 0x0fU];
            } else {
                output += character;
            }
        }
    }
    output += '"';
    return output;
}

auto hex(std::span<const std::byte> input) -> std::string {
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    output.reserve(input.size() * 2);
    for (const auto value : input) {
        const auto byte = std::to_integer<unsigned char>(value);
        output.push_back(digits[byte >> 4U]);
        output.push_back(digits[byte & 0x0fU]);
    }
    return output;
}

auto decode_hex(std::string_view input) -> liquid::result<std::array<std::byte, 32>> {
    if (input.size() != 64) {
        return std::unexpected(cli_error(liquid::errc::invalid_argument,
                                         "public key must contain 64 hex characters",
                                         "public key"));
    }
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    auto output = std::array<std::byte, 32>{};
    for (auto index = std::size_t{}; index < output.size(); ++index) {
        const auto high = nibble(input[index * 2]);
        const auto low = nibble(input[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(cli_error(
                liquid::errc::invalid_argument, "public key contains invalid hex", "public key"));
        }
        output[index] = std::byte{static_cast<unsigned char>((high << 4) | low)};
    }
    return output;
}

auto base64_encode(std::span<const std::byte> input) -> std::string {
    constexpr auto alphabet =
        std::string_view{"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/"};
    auto output = std::string{};
    output.reserve(((input.size() + 2) / 3) * 4);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 3) {
        const auto remaining = input.size() - offset;
        auto value = static_cast<unsigned>(std::to_integer<unsigned char>(input[offset])) << 16U;
        if (remaining > 1) {
            value |= static_cast<unsigned>(std::to_integer<unsigned char>(input[offset + 1])) << 8U;
        }
        if (remaining > 2) {
            value |= static_cast<unsigned>(std::to_integer<unsigned char>(input[offset + 2]));
        }
        output.push_back(alphabet[(value >> 18U) & 0x3fU]);
        output.push_back(alphabet[(value >> 12U) & 0x3fU]);
        output.push_back(remaining > 1 ? alphabet[(value >> 6U) & 0x3fU] : '=');
        output.push_back(remaining > 2 ? alphabet[value & 0x3fU] : '=');
    }
    return output;
}

auto base64_value(char character) -> int {
    if (character >= 'A' && character <= 'Z') {
        return character - 'A';
    }
    if (character >= 'a' && character <= 'z') {
        return character - 'a' + 26;
    }
    if (character >= '0' && character <= '9') {
        return character - '0' + 52;
    }
    if (character == '+') {
        return 62;
    }
    if (character == '/') {
        return 63;
    }
    return -1;
}

auto base64_decode(std::string_view input) -> liquid::result<std::vector<std::byte>> {
    if (input.empty() || input.size() % 4 != 0) {
        return std::unexpected(
            cli_error(liquid::errc::invalid_argument, "invalid base64 record", "record"));
    }
    auto output = std::vector<std::byte>{};
    output.reserve((input.size() / 4) * 3);
    for (auto offset = std::size_t{}; offset < input.size(); offset += 4) {
        auto values = std::array<int, 4>{};
        for (auto index = std::size_t{}; index < values.size(); ++index) {
            values[index] = input[offset + index] == '=' ? 0 : base64_value(input[offset + index]);
            if (values[index] < 0) {
                return std::unexpected(
                    cli_error(liquid::errc::invalid_argument, "invalid base64 record", "record"));
            }
        }
        const auto final = offset + 4 == input.size();
        const auto padding = input[offset + 3] == '=' ? (input[offset + 2] == '=' ? 2U : 1U) : 0U;
        if ((!final && padding != 0) || input[offset] == '=' || input[offset + 1] == '=' ||
            (input[offset + 2] == '=' && input[offset + 3] != '=') ||
            (padding == 1 && (values[2] & 0x03) != 0) ||
            (padding == 2 && (values[1] & 0x0f) != 0)) {
            return std::unexpected(
                cli_error(liquid::errc::invalid_argument, "invalid base64 record", "record"));
        }
        const auto value =
            (static_cast<unsigned>(values[0]) << 18U) | (static_cast<unsigned>(values[1]) << 12U) |
            (static_cast<unsigned>(values[2]) << 6U) | static_cast<unsigned>(values[3]);
        output.push_back(std::byte{static_cast<unsigned char>(value >> 16U)});
        if (padding < 2) {
            output.push_back(std::byte{static_cast<unsigned char>(value >> 8U)});
        }
        if (padding == 0) {
            output.push_back(std::byte{static_cast<unsigned char>(value)});
        }
    }
    return output;
}

auto read_stream(std::istream& input, std::string context)
    -> liquid::result<std::vector<std::byte>> {
    try {
        auto text = std::string{std::istreambuf_iterator<char>{input}, {}};
        if (input.bad()) {
            return std::unexpected(
                cli_error(liquid::errc::io, "cannot read input", std::move(context)));
        }
        const auto* first = reinterpret_cast<const std::byte*>(text.data());
        return std::vector<std::byte>{first, first + text.size()};
    } catch (const std::exception& exception) {
        return std::unexpected(cli_error(liquid::errc::io, exception.what(), std::move(context)));
    }
}

auto read_file(const std::filesystem::path& path) -> liquid::result<std::vector<std::byte>> {
    std::ifstream input(path, std::ios::binary);
    if (!input) {
        return std::unexpected(cli_error(liquid::errc::io, "cannot open file", path.string()));
    }
    return read_stream(input, path.string());
}

auto write_file(const std::filesystem::path& path, std::span<const std::byte> contents)
    -> liquid::result<void> {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) {
        return std::unexpected(
            cli_error(liquid::errc::io, "cannot open output file", path.string()));
    }
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    if (!output) {
        return std::unexpected(
            cli_error(liquid::errc::io, "cannot write output file", path.string()));
    }
    return {};
}

auto read_private_key(const std::filesystem::path& path)
    -> liquid::result<std::array<std::byte, 32>> {
    if (!std::filesystem::is_regular_file(path)) {
        return std::unexpected(
            cli_error(liquid::errc::unauthorized, "key file is not a regular file", path.string()));
    }
#if !defined(_WIN32)
    struct stat status{};
    if (stat(path.c_str(), &status) != 0) {
        return std::unexpected(cli_error(liquid::errc::io, "cannot stat key file", path.string()));
    }
    const auto mode = static_cast<unsigned>(status.st_mode & 0777U);
    if (mode != 0600U) {
        auto context = std::ostringstream{};
        context << path.string() << " mode " << std::oct << mode;
        return std::unexpected(
            cli_error(liquid::errc::unauthorized, "key file must have mode 0600", context.str()));
    }
#endif
    auto contents = read_file(path);
    if (!contents) {
        return std::unexpected(contents.error());
    }
    if (contents->size() != 32) {
        return std::unexpected(cli_error(liquid::errc::invalid_argument,
                                         "key file must contain exactly 32 bytes",
                                         path.string()));
    }
    auto output = std::array<std::byte, 32>{};
    std::ranges::copy(*contents, output.begin());
    return output;
}

auto trim_ascii_space(std::span<const std::byte> input) -> std::string_view {
    auto text = std::string_view{reinterpret_cast<const char*>(input.data()), input.size()};
    while (!text.empty() && (text.front() == ' ' || text.front() == '\t' || text.front() == '\r' ||
                             text.front() == '\n')) {
        text.remove_prefix(1);
    }
    while (!text.empty() && (text.back() == ' ' || text.back() == '\t' || text.back() == '\r' ||
                             text.back() == '\n')) {
        text.remove_suffix(1);
    }
    return text;
}

auto public_key_command(int argc, char** argv) -> int {
    auto lake = std::string{};
    auto json = false;
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json" && !json) {
            json = true;
            continue;
        }
        if (argument == "--lake" && lake.empty() && index + 1 < argc) {
            lake = argv[++index];
            continue;
        }
        return usage("secret public-key --lake <url> [--json]");
    }
    if (lake.empty()) {
        return usage("secret public-key --lake <url> [--json]");
    }
    auto bearer = std::string{};
    if (const auto* token = std::getenv("LIQUID_TOKEN")) {
        bearer = token;
    }
    auto fetched = liquid::runtime::block_on(
        liquid::fetch_sealing_public_key(std::move(lake), std::move(bearer)));
    if (!fetched) {
        return fail(fetched.error());
    }
    const auto key = hex(fetched->key);
    if (json) {
        std::println("{{\"kid\":{},\"public_key\":\"{}\",\"algorithm\":\"x25519\"}}",
                     json_string(fetched->kid),
                     key);
        return 0;
    }
    std::println("kid={}", fetched->kid);
    std::println("public_key={}", key);
    return 0;
}

struct seal_options {
    std::string lake;
    std::string public_key;
    std::string kid;
    std::string cluster;
    std::optional<std::string> namespace_;
    std::optional<std::string> name;
    std::optional<std::filesystem::path> value_file;
    std::optional<std::filesystem::path> output;
};

auto seal_usage() -> int {
    return usage("secret seal (--lake <url> | --public-key <hex> --kid <id>) --cluster <name> "
                 "[--namespace <ns>] [--name <name>] "
                 "[--value-file <path>] [--out <file>]  (the value from the file, else stdin)");
}

auto parse_seal_options(int argc, char** argv) -> liquid::result<seal_options> {
    auto output = seal_options{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (index + 1 >= argc) {
            return std::unexpected(cli_error(
                liquid::errc::invalid_argument, "option requires a value", "secret seal"));
        }
        const auto value = std::string{argv[++index]};
        if (argument == "--lake" && output.lake.empty()) {
            output.lake = value;
        } else if (argument == "--public-key" && output.public_key.empty()) {
            output.public_key = value;
        } else if (argument == "--kid" && output.kid.empty()) {
            output.kid = value;
        } else if (argument == "--cluster" && output.cluster.empty()) {
            output.cluster = value;
        } else if (argument == "--namespace" && !output.namespace_) {
            output.namespace_ = value;
        } else if (argument == "--name" && !output.name) {
            output.name = value;
        } else if (argument == "--value") {
            // A value on the command line is readable by every local user in ps and
            // /proc/<pid>/cmdline, and lands in shell history (L-12, liquid#394).
            return std::unexpected(cli_error(liquid::errc::invalid_argument,
                                             "--value is not accepted: pass the value on stdin "
                                             "or with --value-file",
                                             "secret seal"));
        } else if (argument == "--value-file" && !output.value_file) {
            output.value_file = value;
        } else if (argument == "--out" && !output.output) {
            output.output = value;
        } else {
            return std::unexpected(cli_error(
                liquid::errc::invalid_argument, "invalid or repeated option", "secret seal"));
        }
    }
    const auto remote = !output.lake.empty();
    const auto direct = !output.public_key.empty() && !output.kid.empty();
    if (remote == direct || output.cluster.empty() ||
        (remote && (!output.public_key.empty() || !output.kid.empty())) ||
        (!remote && (output.public_key.empty() || output.kid.empty()))) {
        return std::unexpected(
            cli_error(liquid::errc::invalid_argument, "invalid option combination", "secret seal"));
    }
    return output;
}

auto seal_command(int argc, char** argv) -> int {
    auto options = parse_seal_options(argc, argv);
    if (!options) {
        return seal_usage();
    }
    auto recipient = liquid::sealing_public_key{};
    if (!options->lake.empty()) {
        auto bearer = std::string{};
        if (const auto* token = std::getenv("LIQUID_TOKEN")) {
            bearer = token;
        }
        auto fetched = liquid::runtime::block_on(
            liquid::fetch_sealing_public_key(std::move(options->lake), std::move(bearer)));
        if (!fetched) {
            return fail(fetched.error());
        }
        recipient = std::move(*fetched);
    } else {
        auto key = decode_hex(options->public_key);
        if (!key) {
            return fail(key.error());
        }
        recipient = {.kid = std::move(options->kid), .key = *key};
    }
    auto plaintext = liquid::result<std::vector<std::byte>>{};
    if (options->value_file) {
        plaintext = read_file(*options->value_file);
    } else {
        plaintext = read_stream(std::cin, "stdin");
    }
    if (!plaintext) {
        return fail(plaintext.error());
    }
    auto sealed =
        liquid::seal_secret(recipient,
                            liquid::seal_scope{.cluster = std::move(options->cluster),
                                               .namespace_ = std::move(options->namespace_),
                                               .name = std::move(options->name)},
                            *plaintext);
    if (!sealed) {
        return fail(sealed.error());
    }
    auto encoded = liquid::encode_sealed_secret(*sealed);
    if (!encoded) {
        return fail(encoded.error());
    }
    if (options->output) {
        auto written = write_file(*options->output, *encoded);
        return written ? 0 : fail(written.error());
    }
    std::println("{}", base64_encode(*encoded));
    return 0;
}

struct unseal_options {
    std::filesystem::path private_key;
    std::string cluster;
    std::optional<std::string> namespace_;
    std::optional<std::string> name;
    std::string record;
};

auto unseal_usage() -> int {
    return usage("secret unseal --private-key-file <path> --cluster <name> [--namespace <ns>] "
                 "[--name <name>] (<record file> | - | <base64>)");
}

auto parse_unseal_options(int argc, char** argv) -> liquid::result<unseal_options> {
    auto output = unseal_options{};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--private-key-file" || argument == "--cluster" ||
            argument == "--namespace" || argument == "--name") {
            if (index + 1 >= argc) {
                return std::unexpected(cli_error(
                    liquid::errc::invalid_argument, "option requires a value", "secret unseal"));
            }
            const auto value = std::string{argv[++index]};
            if (argument == "--private-key-file" && output.private_key.empty()) {
                output.private_key = value;
            } else if (argument == "--cluster" && output.cluster.empty()) {
                output.cluster = value;
            } else if (argument == "--namespace" && !output.namespace_) {
                output.namespace_ = value;
            } else if (argument == "--name" && !output.name) {
                output.name = value;
            } else {
                return std::unexpected(cli_error(
                    liquid::errc::invalid_argument, "invalid or repeated option", "secret unseal"));
            }
            continue;
        }
        if (!output.record.empty()) {
            return std::unexpected(cli_error(
                liquid::errc::invalid_argument, "multiple records supplied", "secret unseal"));
        }
        output.record = argument;
    }
    if (output.private_key.empty() || output.cluster.empty() || output.record.empty()) {
        return std::unexpected(cli_error(
            liquid::errc::invalid_argument, "required option is missing", "secret unseal"));
    }
    return output;
}

auto read_record(std::string_view argument) -> liquid::result<std::vector<std::byte>> {
    if (argument == "-") {
        auto input = read_stream(std::cin, "stdin");
        if (!input) {
            return std::unexpected(input.error());
        }
        if (liquid::decode_sealed_secret(*input)) {
            return input;
        }
        return base64_decode(trim_ascii_space(*input));
    }
    const auto path = std::filesystem::path{argument};
    auto code = std::error_code{};
    if (std::filesystem::is_regular_file(path, code)) {
        return read_file(path);
    }
    return base64_decode(argument);
}

auto unseal_command(int argc, char** argv) -> int {
    auto options = parse_unseal_options(argc, argv);
    if (!options) {
        return unseal_usage();
    }
    auto private_key = read_private_key(options->private_key);
    if (!private_key) {
        return fail(private_key.error());
    }
    auto record = read_record(options->record);
    if (!record) {
        return fail(record.error());
    }
    auto decoded = liquid::decode_sealed_secret(*record);
    if (!decoded) {
        return fail(decoded.error());
    }
    auto plaintext =
        liquid::unseal_secret(*private_key,
                              *decoded,
                              liquid::seal_scope{.cluster = std::move(options->cluster),
                                                 .namespace_ = std::move(options->namespace_),
                                                 .name = std::move(options->name)});
    if (!plaintext) {
        return fail(plaintext.error());
    }
    std::cout.write(reinterpret_cast<const char*>(plaintext->data()),
                    static_cast<std::streamsize>(plaintext->size()));
    if (!std::cout) {
        return fail(cli_error(liquid::errc::io, "cannot write plaintext", "stdout"));
    }
    return 0;
}

} // namespace

auto secret(int argc, char** argv) -> int {
    if (argc < 1) {
        return usage("secret requires public-key, seal, or unseal");
    }
    const auto command = std::string_view{argv[0]};
    if (command == "public-key") {
        return public_key_command(argc - 1, argv + 1);
    }
    if (command == "seal") {
        return seal_command(argc - 1, argv + 1);
    }
    if (command == "unseal") {
        return unseal_command(argc - 1, argv + 1);
    }
    return usage("secret requires public-key, seal, or unseal");
}

} // namespace liquid_cli
