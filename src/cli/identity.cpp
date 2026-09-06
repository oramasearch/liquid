#include <format>
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

} // namespace

auto identity(int argc, char** argv) -> int {
    auto command = std::string_view{"show"};
    auto json = false;
    auto custody = std::string{"software"};
    for (auto index = 0; index < argc; ++index) {
        const auto argument = std::string_view{argv[index]};
        if (argument == "--json") {
            json = true;
        } else if (argument == "--to" && index + 1 < argc) {
            custody = argv[++index];
        } else if (argument == "show" || argument == "rotate") {
            command = argument;
        } else {
            return usage("identity [show|rotate [--to software|fido2|piv:9c]] [--json]");
        }
    }
    if (command == "show" && custody != "software") {
        return usage("identity show does not accept --to");
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
    print_identity(signer->identity(), json);
    return 0;
}

} // namespace liquid_cli
