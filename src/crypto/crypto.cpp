#include "crypto.hpp"

#include <cstring>
#include <string>

#if defined(__APPLE__)
#include <stdlib.h>
#elif defined(__linux__)
#include <cerrno>
#include <sys/random.h>
#elif defined(_WIN32)
#include <windows.h>
#include <bcrypt.h>
#endif

#include "blake3.h"
#include "monocypher.h"

namespace liquid::crypto_detail {
namespace {

constexpr std::size_t nonce_size = 24;
constexpr std::size_t tag_size = 16;

error crypto_error(errc code, std::string message, std::string context) {
  return {code, std::move(message), std::move(context)};
}

}  // namespace

result<void> random_bytes(std::span<std::byte> bytes) {
#if defined(__APPLE__)
  arc4random_buf(bytes.data(), bytes.size());
  return {};
#elif defined(__linux__)
  std::size_t offset = 0;
  while (offset < bytes.size()) {
    const auto count = getrandom(bytes.data() + offset, bytes.size() - offset, 0);
    if (count < 0) {
      if (errno == EINTR)
        continue;
      return std::unexpected(crypto_error(errc::unavailable, "operating-system random source failed",
                                          "getrandom"));
    }
    if (count == 0)
      return std::unexpected(crypto_error(errc::unavailable, "operating-system random source ended",
                                          "getrandom"));
    offset += static_cast<std::size_t>(count);
  }
  return {};
#elif defined(_WIN32)
  if (BCryptGenRandom(nullptr, reinterpret_cast<PUCHAR>(bytes.data()),
                      static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0) {
    return std::unexpected(crypto_error(errc::unavailable, "operating-system random source failed",
                                        "BCryptGenRandom"));
  }
  return {};
#else
#error "liquid encrypted stores require an operating-system CSPRNG implementation"
#endif
}

void wipe(std::span<std::byte> bytes) noexcept {
  crypto_wipe(bytes.data(), bytes.size());
}

result<key> random_key() {
  key output{};
  auto filled = random_bytes(output);
  if (!filled)
    return std::unexpected(filled.error());
  return output;
}

result<key> derive_key(const key &input, std::string_view context) {
  key output{};
  blake3_hasher hasher;
  const std::string label(context);
  blake3_hasher_init_derive_key(&hasher, label.c_str());
  blake3_hasher_update(&hasher, input.data(), input.size());
  blake3_hasher_finalize(&hasher, reinterpret_cast<std::uint8_t *>(output.data()), output.size());
  return output;
}

result<domain_keys> derive_domain_keys(const key &data_key) {
  auto mac = derive_key(data_key, "liquid store chunk identifier v1");
  if (!mac)
    return std::unexpected(mac.error());
  auto encryption = derive_key(data_key, "liquid store chunk encryption v1");
  if (!encryption)
    return std::unexpected(encryption.error());
  return domain_keys{*mac, *encryption};
}

result<digest> keyed_digest(const key &key_bytes, std::span<const std::byte> input) {
  digest output{algorithm::blake3_keyed, {}};
  blake3_hasher hasher;
  blake3_hasher_init_keyed(&hasher, reinterpret_cast<const std::uint8_t *>(key_bytes.data()));
  blake3_hasher_update(&hasher, input.data(), input.size());
  blake3_hasher_finalize(&hasher, reinterpret_cast<std::uint8_t *>(output.bytes.data()),
                         output.bytes.size());
  return output;
}

result<std::vector<std::byte>> seal(const key &key_bytes, std::span<const std::byte> ad,
                                    std::span<const std::byte> plaintext) {
  std::vector<std::byte> output(nonce_size + tag_size + plaintext.size());
  auto nonce = std::span<std::byte>(output).first(nonce_size);
  auto random = random_bytes(nonce);
  if (!random)
    return std::unexpected(random.error());
  crypto_aead_lock(reinterpret_cast<std::uint8_t *>(output.data() + nonce_size + tag_size),
                   reinterpret_cast<std::uint8_t *>(output.data() + nonce_size),
                   reinterpret_cast<const std::uint8_t *>(key_bytes.data()),
                   reinterpret_cast<const std::uint8_t *>(nonce.data()),
                   reinterpret_cast<const std::uint8_t *>(ad.data()), ad.size(),
                   reinterpret_cast<const std::uint8_t *>(plaintext.data()), plaintext.size());
  return output;
}

result<std::vector<std::byte>> open(const key &key_bytes, std::span<const std::byte> ad,
                                    std::span<const std::byte> boxed) {
  if (boxed.size() < nonce_size + tag_size)
    return std::unexpected(crypto_error(errc::corrupt, "truncated authenticated ciphertext", "aead"));
  std::vector<std::byte> output(boxed.size() - nonce_size - tag_size);
  const auto *nonce = reinterpret_cast<const std::uint8_t *>(boxed.data());
  const auto *tag = reinterpret_cast<const std::uint8_t *>(boxed.data() + nonce_size);
  const auto *ciphertext = reinterpret_cast<const std::uint8_t *>(boxed.data() + nonce_size + tag_size);
  if (crypto_aead_unlock(reinterpret_cast<std::uint8_t *>(output.data()), tag,
                         reinterpret_cast<const std::uint8_t *>(key_bytes.data()), nonce,
                         reinterpret_cast<const std::uint8_t *>(ad.data()), ad.size(), ciphertext,
                         output.size()) != 0) {
    return std::unexpected(crypto_error(errc::corrupt, "authenticated decryption failed", "aead"));
  }
  return output;
}

}  // namespace liquid::crypto_detail
