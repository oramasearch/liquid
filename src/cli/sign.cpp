#include <filesystem>
#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/projection.hpp"
#include "liquid/repository.hpp"
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

auto canonical_digest(std::string_view input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    if (input.size() < 24) {
        output.push_back(std::byte{static_cast<unsigned char>(0x60U + input.size())});
    } else {
        output.push_back(std::byte{0x78});
        output.push_back(std::byte{static_cast<unsigned char>(input.size())});
    }
    const auto bytes = std::as_bytes(std::span{input});
    output.insert(output.end(), bytes.begin(), bytes.end());
    return output;
}

} // namespace

auto sign(int argc, char** argv) -> int {
    if (argc == 2 && std::string_view{argv[0]} == "verify") {
        auto verified = liquid::runtime::block_on(
            liquid::verify_platform_signature(std::filesystem::path{argv[1]}));
        if (!verified) {
            return fail(verified.error());
        }
        std::println("verified {} {} identity={}",
                     liquid::to_string(verified->platform),
                     liquid::to_string(verified->signed_digest),
                     verified->signing_identity_fingerprint);
        return 0;
    }
    const auto platform = std::string_view{argv[0]};
    if (platform == "macos" || platform == "windows") {
        if (argc < 4) {
            return usage("sign <macos|windows> <artifact> --identity <custody-uri>");
        }
        const auto artifact = std::filesystem::path{argv[1]};
        auto options = liquid::platform_signing_options{};
        for (auto index = 2; index < argc; ++index) {
            const auto argument = std::string_view{argv[index]};
            const auto value = [&](std::string_view option) -> liquid::result<std::string> {
                if (++index == argc) {
                    return std::unexpected(liquid::error{liquid::errc::invalid_argument,
                                                         std::string{option} + " requires a value",
                                                         "sign"});
                }
                return std::string{argv[index]};
            };
            if (argument == "--identity" || argument == "--timestamp-server" ||
                argument == "--out") {
                auto selected = value(argument);
                if (!selected) {
                    return fail(selected.error());
                }
                if (argument == "--identity") {
                    options.identity = std::move(*selected);
                } else if (argument == "--timestamp-server") {
                    options.timestamp_server = std::move(*selected);
                } else {
                    options.output = std::move(*selected);
                }
                continue;
            }
            if (argument == "--no-notarize") {
                options.notarize = false;
                continue;
            }
            return usage("sign received an unknown option");
        }
        if (options.identity.empty()) {
            return usage("sign requires --identity <custody-uri>");
        }
        const auto working_tree = std::filesystem::current_path();
        auto repository = liquid::runtime::block_on(liquid::repository::open_build(working_tree));
        if (!repository) {
            repository = liquid::runtime::block_on(liquid::repository::open(working_tree));
        }
        if (!repository) {
            return fail(repository.error());
        }
        auto request_platform = platform == "macos" ? liquid::platform_signing_system::macos
                                                    : liquid::platform_signing_system::windows;
        const auto expected_extension =
            request_platform == liquid::platform_signing_system::macos
                ? (artifact.extension() == ".pkg" || artifact.extension() == ".dmg")
                : artifact.extension() == ".msix";
        if (!expected_extension) {
            return usage("sign platform does not match the artifact extension");
        }
        auto report = liquid::runtime::block_on(
            liquid::sign_platform(*repository, artifact, std::move(options)));
        if (!report) {
            return fail(report.error());
        }
        std::println("artifact: {} {}",
                     report->artifact.path.string(),
                     liquid::to_string(report->artifact.digest));
        std::println("identity: {}", report->signing_identity_fingerprint);
        std::println("attestation: {}", report->attestation.string());
        if (!report->notarization_ticket.empty()) {
            std::println("notarization-ticket: {}", report->notarization_ticket);
        } else if (report->notarization_skipped) {
            std::println("notarization: skipped (LIQUID_APPLE_NOTARY_* is incomplete)");
        }
        return 0;
    }
    if (argc != 2) {
        return usage("sign <drip|change|artifact> <digest>");
    }
    const auto purpose = std::string_view{argv[0]};
    if (purpose != "drip" && purpose != "change" && purpose != "artifact") {
        return usage("sign <drip|change|artifact> <digest>");
    }
    auto id = liquid::parse_digest(argv[1]);
    if (!id) {
        return fail(id.error());
    }
    auto body = canonical_digest(argv[1]);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return fail(signer.error());
    }
    auto signed_value =
        liquid::runtime::block_on(signer->sign(std::string{purpose}, std::move(body)));
    if (!signed_value) {
        return fail(signed_value.error());
    }
    auto encoded = liquid::encode_attestation_signature(*signed_value);
    if (!encoded) {
        return fail(encoded.error());
    }
    std::println("signature={}", hex(*encoded));
    std::println("signer={}", signer->identity().taap_id);
    return 0;
}

} // namespace liquid_cli
