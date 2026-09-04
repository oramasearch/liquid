#pragma once

#include <array>
#include <cstddef>
#include <span>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/hash.hpp"

namespace liquid::crypto_detail {

using key = std::array<std::byte, 32>;

struct domain_keys {
  key chunk_mac;
  key chunk_encryption;
};

[[nodiscard]] result<void> random_bytes(std::span<std::byte> bytes);
void wipe(std::span<std::byte> bytes) noexcept;
[[nodiscard]] result<key> random_key();
[[nodiscard]] result<key> derive_key(const key &input, std::string_view context);
[[nodiscard]] result<domain_keys> derive_domain_keys(const key &data_key);
[[nodiscard]] result<digest> keyed_digest(const key &key, std::span<const std::byte> input);
[[nodiscard]] result<std::vector<std::byte>> seal(const key &key,
                                                   std::span<const std::byte> ad,
                                                   std::span<const std::byte> plaintext);
[[nodiscard]] result<std::vector<std::byte>> open(const key &key,
                                                   std::span<const std::byte> ad,
                                                   std::span<const std::byte> boxed);

}  // namespace liquid::crypto_detail
