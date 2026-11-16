// Sealed secrets (liquid #463): the sealed-secret v1 record Lake and the operator hand back, its
// unsealing, and Lake's answer to GET /v1/sealing_public_key (src/secret/sealed_secret.cpp).
//
//   0  a sealed-secret record: decode, encode, decode again; the second decode must equal the first
//   1  a sealed-secret record, unsealed against its own scope with the recipient key of the test
//      fixture (tests/fixtures/sealed-secret/recipient-private.hex, test material only), so the
//      fixture's record unseals and its mutations reach the X25519 exchange, HKDF and the
//      authenticated decryption
//   2  the body of Lake's sealing-public-key response, through the injected requester

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "fuzz.hpp"
#include "liquid/runtime.hpp"
#include "liquid/secret.hpp"
#include "net/http_client.hpp"
#include "secret/secret.hpp"

namespace net = liquid::net_detail;

namespace {

auto respond(std::vector<std::byte> body) -> liquid::runtime::task<net::http_response> {
    co_return net::http_response{200, std::move(body)};
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 3);
    if (!input) {
        return 0;
    }
    const auto payload = input->payload;
    switch (input->decoder) {
    case 0: {
        const auto decoded = liquid::decode_sealed_secret(payload);
        if (!decoded) {
            break;
        }
        const auto encoded = liquid::encode_sealed_secret(*decoded);
        if (encoded) {
            const auto again = liquid::decode_sealed_secret(*encoded);
            liquid::fuzz::require(again && *again == *decoded);
        }
        break;
    }
    case 1: {
        const auto decoded = liquid::decode_sealed_secret(payload);
        if (!decoded) {
            break;
        }
        constexpr auto key_text = std::string_view{"0123456789abcdef0123456789abcdef"};
        auto private_key = std::array<std::byte, 32>{};
        for (std::size_t index = 0; index < private_key.size(); ++index) {
            private_key[index] = static_cast<std::byte>(key_text[index]);
        }
        (void)liquid::unseal_secret(private_key, *decoded, decoded->scope);
        break;
    }
    default: {
        const auto body = std::vector<std::byte>(payload.begin(), payload.end());
        (void)liquid::runtime::block_on(liquid::secret_detail::fetch_sealing_public_key_with(
            "https://lake.invalid", {}, [&body](net::http_request) { return respond(body); }));
        break;
    }
    }
    return 0;
}
