#include <gtest/gtest.h>

#include <array>
#include <cstddef>
#include <cstring>
#include <span>
#include <string_view>
#include <vector>

#include "monocypher.h"
#include "crypto.hpp"

namespace {

std::vector<std::byte> hex(std::string_view input) {
  std::vector<std::byte> output;
  output.reserve(input.size() / 2);
  for (std::size_t index = 0; index < input.size(); index += 2) {
    const auto value = [](char digit) {
      return digit >= 'a' ? digit - 'a' + 10 : digit - '0';
    };
    output.push_back(std::byte{static_cast<unsigned char>((value(input[index]) << 4) |
                                                           value(input[index + 1]))});
  }
  return output;
}

std::string hex_text(std::span<const std::byte> input) {
  static constexpr char digits[] = "0123456789abcdef";
  std::string output;
  output.reserve(input.size() * 2);
  for (const auto byte : input) {
    const auto value = std::to_integer<unsigned char>(byte);
    output.push_back(digits[value >> 4]);
    output.push_back(digits[value & 15]);
  }
  return output;
}

}  // namespace

TEST(Crypto, XChaCha20Poly1305KnownAnswer) {
  // Fixed XChaCha20-Poly1305 known-answer vector for the Monocypher 4.0.3 API.
  const auto key = hex("79c99798ac67300bbb2704c95c341e3245f3d0e3b1d7c55f7a69f4ed5e9bc67b");
  const auto nonce = hex("b33ffd3096479bcfbc9aee49417688a0a2554f8d95389419");
  const auto ad = hex("c0875924c1c7987947deafd8780acf49");
  const auto plaintext = hex("4c616469657320616e642047656e746c656d656e206f662074686520636c617373206f66202739393a20");
  const auto expected = "5dc5addff09929e6c80448a07b88c377e6547fc47c842257752e824487b4f588845e71f6b8252c42bcae7f922bae822a472cf67670def3fa8346";
  std::vector<std::byte> ciphertext(plaintext.size());
  std::array<std::byte, 16> tag{};
  crypto_aead_lock(reinterpret_cast<std::uint8_t*>(ciphertext.data()),
                   reinterpret_cast<std::uint8_t*>(tag.data()),
                   reinterpret_cast<const std::uint8_t*>(key.data()),
                   reinterpret_cast<const std::uint8_t*>(nonce.data()),
                   reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size(),
                   reinterpret_cast<const std::uint8_t*>(plaintext.data()), plaintext.size());
  std::vector<std::byte> combined;
  combined.insert(combined.end(), ciphertext.begin(), ciphertext.end());
  combined.insert(combined.end(), tag.begin(), tag.end());
  EXPECT_EQ(hex_text(combined), expected);
  std::vector<std::byte> decrypted(plaintext.size());
  EXPECT_EQ(crypto_aead_unlock(reinterpret_cast<std::uint8_t*>(decrypted.data()),
                               reinterpret_cast<const std::uint8_t*>(tag.data()),
                               reinterpret_cast<const std::uint8_t*>(key.data()),
                               reinterpret_cast<const std::uint8_t*>(nonce.data()),
                               reinterpret_cast<const std::uint8_t*>(ad.data()), ad.size(),
                               reinterpret_cast<const std::uint8_t*>(ciphertext.data()), ciphertext.size()), 0);
  EXPECT_EQ(decrypted, plaintext);
}

TEST(Crypto, AeadEnvelopeRoundTrips) {
  liquid::crypto_detail::key key{};
  for (std::size_t index = 0; index < key.size(); ++index)
    key[index] = std::byte{static_cast<unsigned char>(index)};
  const auto ad = hex("00010203");
  const auto plaintext = hex("00112233445566778899aabbccddeeff");
  const auto boxed = liquid::crypto_detail::seal(key, ad, plaintext);
  ASSERT_TRUE(boxed.has_value()) << boxed.error().message;
  const auto opened = liquid::crypto_detail::open(key, ad, *boxed);
  ASSERT_TRUE(opened.has_value()) << opened.error().message;
  EXPECT_EQ(*opened, plaintext);
}
