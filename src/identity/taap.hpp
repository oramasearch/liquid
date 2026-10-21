#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/identity.hpp"

namespace liquid::identity_detail {

[[nodiscard]] LIQUID_EXPORT auto sha512_256(std::span<const std::byte> input)
    -> std::array<std::byte, 32>;
[[nodiscard]] auto base32(std::span<const std::byte> input) -> std::string;
[[nodiscard]] auto base64(std::span<const std::byte> input) -> std::string;
[[nodiscard]] auto identifier(std::span<const std::byte> public_key,
                              std::string_view created_at,
                              std::string_view key_label) -> result<actor>;
[[nodiscard]] auto identifier(std::span<const std::byte> public_key,
                              std::string_view created_at,
                              std::string_view key_label,
                              std::string_view algorithm,
                              std::string_view custody) -> result<actor>;
[[nodiscard]] auto key_fingerprint(std::span<const std::byte> public_key,
                                   std::string_view created_at,
                                   std::string_view key_label,
                                   std::string_view algorithm = "ed25519",
                                   std::string_view custody = {}) -> result<std::string>;
[[nodiscard]] auto canonical_signature(const signature& value) -> result<std::vector<std::byte>>;
[[nodiscard]] auto parse_signature(std::span<const std::byte> input) -> result<signature>;
[[nodiscard]] auto canonical_document(const identity_document& document, bool include_signatures)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto parse_document(std::span<const std::byte> input)
    -> result<identity_document>;
[[nodiscard]] auto signing_input(std::string_view artifact_type,
                                 std::span<const std::byte> canonical_body)
    -> std::vector<std::byte>;
[[nodiscard]] auto now() -> std::string;
// Verifies a signature against exactly the key in `pinned`, which the caller has already bound to
// an identity (for example a worker identity document an operator configured). It never consults
// the local identity store and never accepts a key named only by the signature.
[[nodiscard]] LIQUID_EXPORT auto verify_with_pinned_key(const actor& pinned,
                                                        std::string_view artifact_type,
                                                        std::span<const std::byte> canonical_body,
                                                        const signature& signed_value)
    -> result<bool>;
[[nodiscard]] LIQUID_EXPORT auto local_signer() -> runtime::task<signer>;

} // namespace liquid::identity_detail
