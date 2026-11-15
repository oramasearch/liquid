// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <span>
#include <vector>

#include "liquid/error.hpp"

namespace liquid::crypto {

enum class digest_kind { sha256, sha384 };
enum class signature_kind { rsa_pkcs1, rsa_pss, ecdsa };
enum class curve_kind { none, p256, p384 };

struct public_key {
    signature_kind kind = signature_kind::rsa_pkcs1;
    curve_kind curve = curve_kind::none;
    std::vector<std::byte> first;
    std::vector<std::byte> second;
};

struct signature_parameters {
    signature_kind kind = signature_kind::rsa_pkcs1;
    digest_kind digest = digest_kind::sha256;
    std::size_t pss_salt_length = 0;
};

auto digest(digest_kind kind, std::span<const std::byte> input) -> result<std::vector<std::byte>>;

auto verify_signature(const public_key& key,
                      const signature_parameters& parameters,
                      std::span<const std::byte> message,
                      std::span<const std::byte> signature) -> result<void>;

} // namespace liquid::crypto
