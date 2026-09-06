#include <print>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace liquid_cli {
namespace {

auto fail(const liquid::error& error) -> int {
    std::println(
        stderr, "error: {}: {} ({})", liquid::to_string(error.code), error.message, error.context);
    return 1;
}

auto usage() -> int {
    std::println(stderr, "error: usage: sign <drip|change|artifact> <digest> ()");
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
    if (argc != 2) {
        return usage();
    }
    const auto purpose = std::string_view{argv[0]};
    if (purpose != "drip" && purpose != "change" && purpose != "artifact") {
        return usage();
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
    auto signed_value = liquid::runtime::block_on(signer->sign(std::string{purpose}, std::move(body)));
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
