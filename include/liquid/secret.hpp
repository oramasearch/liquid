// SPDX-License-Identifier: MIT

#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

struct sealing_public_key {
    std::string kid;
    std::array<std::byte, 32> key;

    auto operator==(const sealing_public_key&) const -> bool = default;
};

struct sealing_keypair {
    sealing_public_key public_key;
    std::array<std::byte, 32> private_key;

    auto operator==(const sealing_keypair&) const -> bool = default;
};

struct seal_scope {
    std::string cluster;
    std::optional<std::string> namespace_;
    std::optional<std::string> name;

    auto operator==(const seal_scope&) const -> bool = default;
};

struct sealed_secret {
    std::uint64_t v;
    std::string kid;
    std::array<std::byte, 32> epk;
    std::array<std::byte, 24> nonce;
    seal_scope scope;
    std::vector<std::byte> ciphertext;

    auto operator==(const sealed_secret&) const -> bool = default;
};

[[nodiscard]] LIQUID_EXPORT auto encode_sealed_secret(const sealed_secret& value)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto decode_sealed_secret(std::span<const std::byte> input)
    -> result<sealed_secret>;
[[nodiscard]] LIQUID_EXPORT auto seal_secret(const sealing_public_key& recipient,
                                             seal_scope scope,
                                             std::span<const std::byte> plaintext)
    -> result<sealed_secret>;
[[nodiscard]] LIQUID_EXPORT auto unseal_secret(std::span<const std::byte, 32> private_key,
                                               const sealed_secret& value,
                                               const seal_scope& expected)
    -> result<std::vector<std::byte>>;
[[nodiscard]] LIQUID_EXPORT auto generate_sealing_keypair() -> result<sealing_keypair>;
[[nodiscard]] LIQUID_EXPORT auto fetch_sealing_public_key(std::string lake_url,
                                                          std::string bearer = {})
    -> runtime::task<sealing_public_key>;

} // namespace liquid
