#include "liquid/identity.hpp"

#include <array>
#include <cstring>
#include <fstream>
#include <limits>
#include <map>
#include <sstream>
#include <utility>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "crypto/crypto.hpp"
#include "monocypher.h"
#include "store/pack.hpp"

namespace liquid {
namespace {

constexpr std::array<std::byte, 8> key_magic{std::byte{'L'}, std::byte{'Q'}, std::byte{'K'},
                                             std::byte{'E'}, std::byte{'Y'}, std::byte{1},
                                             std::byte{0}, std::byte{0}};
constexpr std::string_view key_wrap_context = "liquid store data key wrapping v1";

error key_error(errc code, std::string message, std::string context) {
  return {code, std::move(message), std::move(context)};
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
  for (unsigned index = 0; index != 4; ++index)
    out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t at) {
  if (at + 4 > bytes.size())
    return 0;
  std::uint32_t value = 0;
  for (unsigned index = 0; index != 4; ++index)
    value |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + index])) << (index * 8U);
  return value;
}

bool valid_key_id(std::string_view key_id) {
  return !key_id.empty() && key_id != "." && key_id != ".." &&
         key_id.size() <= std::numeric_limits<std::uint16_t>::max() &&
         key_id.find_first_of("/\\") == std::string_view::npos &&
         key_id.find('\0') == std::string_view::npos;
}

result<void> validate_argon2(argon2_parameters parameters) {
  if (parameters.lanes == 0 || parameters.iterations == 0 ||
      parameters.memory_kib < 8U * parameters.lanes) {
    return std::unexpected(key_error(errc::invalid_argument, "invalid Argon2i parameters",
                                     "passphrase_provider"));
  }
  return {};
}

struct wrapped_data_key {
  std::array<std::byte, 16> salt{};
  argon2_parameters parameters{};
  std::vector<std::byte> boxed;
};

result<wrapped_data_key> parse_wrapped_data_key(std::span<const std::byte> bytes,
                                                std::string_view key_id) {
  constexpr std::size_t header_size = key_magic.size() + 16 + 12;
  constexpr std::size_t boxed_size = 24 + 16 + 32;
  if (bytes.size() != header_size + boxed_size ||
      !std::equal(key_magic.begin(), key_magic.end(), bytes.begin())) {
    return std::unexpected(key_error(errc::corrupt, "invalid wrapped data key", std::string(key_id)));
  }
  wrapped_data_key output;
  std::copy_n(bytes.begin() + key_magic.size(), output.salt.size(), output.salt.begin());
  output.parameters = {get_u32(bytes, 24), get_u32(bytes, 28), get_u32(bytes, 32)};
  auto valid = validate_argon2(output.parameters);
  if (!valid)
    return std::unexpected(key_error(errc::corrupt, "invalid wrapped data-key parameters",
                                     std::string(key_id)));
  output.boxed.assign(bytes.begin() + header_size, bytes.end());
  return output;
}

std::vector<std::byte> encode_wrapped_data_key(const wrapped_data_key& wrapped) {
  std::vector<std::byte> output(key_magic.begin(), key_magic.end());
  output.insert(output.end(), wrapped.salt.begin(), wrapped.salt.end());
  put_u32(output, wrapped.parameters.memory_kib);
  put_u32(output, wrapped.parameters.iterations);
  put_u32(output, wrapped.parameters.lanes);
  output.insert(output.end(), wrapped.boxed.begin(), wrapped.boxed.end());
  return output;
}

runtime::task<void> write_atomic(std::filesystem::path path, std::vector<std::byte> bytes) {
  const auto temporary = path.string() + ".tmp";
  auto written = co_await store_detail::write_file(temporary, std::move(bytes));
  if (!written)
    co_return std::unexpected(written.error());
  try {
    std::filesystem::rename(temporary, path);
  } catch (const std::exception& caught) {
    co_return std::unexpected(key_error(errc::io, caught.what(), path.string()));
  }
  co_return result<void>{};
}

}  // namespace

template <class T>
result<T> unavailable(const char* issue) {
  return std::unexpected(error{errc::unsupported, "not implemented", issue});
}

struct signer::impl {};

signer::signer() noexcept = default;
signer::~signer() = default;
signer::signer(signer&&) noexcept = default;
signer& signer::operator=(signer&&) noexcept = default;

runtime::task<std::vector<std::byte>> signer::sign(std::vector<std::byte>) {
  co_return unavailable<std::vector<std::byte>>("liquid #4");
}

result<bool> verify(const actor&, std::span<const std::byte>, std::span<const std::byte>) {
  return unavailable<bool>("liquid #4");
}

struct passphrase_provider::impl {
  explicit impl(std::string value) : passphrase(std::move(value)) {}
  ~impl() { crypto_wipe(passphrase.data(), passphrase.size()); }
  std::string passphrase;
};

passphrase_provider::passphrase_provider(std::string passphrase)
    : impl_(std::make_unique<impl>(std::move(passphrase))) {}
passphrase_provider::~passphrase_provider() = default;
passphrase_provider::passphrase_provider(passphrase_provider&&) noexcept = default;
passphrase_provider& passphrase_provider::operator=(passphrase_provider&&) noexcept = default;

result<std::array<std::byte, 32>> passphrase_provider::derive(
    std::span<const std::byte> salt, argon2_parameters parameters) const {
  auto valid = validate_argon2(parameters);
  if (!valid)
    return std::unexpected(valid.error());
  if (salt.size() < 8 || salt.size() > std::numeric_limits<std::uint32_t>::max() ||
      impl_->passphrase.size() > std::numeric_limits<std::uint32_t>::max()) {
    return std::unexpected(key_error(errc::invalid_argument, "invalid passphrase or salt length",
                                     "passphrase_provider"));
  }
  std::vector<std::byte> work(static_cast<std::size_t>(parameters.memory_kib) * 1024U);
  std::array<std::byte, 32> output{};
  const crypto_argon2_config config{CRYPTO_ARGON2_I, parameters.memory_kib,
                                    parameters.iterations, parameters.lanes};
  const crypto_argon2_inputs input{
      reinterpret_cast<const std::uint8_t*>(impl_->passphrase.data()),
      reinterpret_cast<const std::uint8_t*>(salt.data()),
      static_cast<std::uint32_t>(impl_->passphrase.size()), static_cast<std::uint32_t>(salt.size())};
  crypto_argon2(reinterpret_cast<std::uint8_t*>(output.data()), output.size(), work.data(), config,
                input, crypto_argon2_no_extras);
  return output;
}

result<std::array<std::byte, 32>> passphrase_provider::derive_kek(
    std::span<const std::byte> salt, argon2_parameters parameters) const {
  return derive(salt, parameters);
}

struct key_file_provider::impl {
  explicit impl(std::array<std::byte, 32> value) : key(value) {}
  ~impl() { crypto_wipe(key.data(), key.size()); }
  std::array<std::byte, 32> key;
};

key_file_provider::key_file_provider(std::array<std::byte, 32> key) noexcept
    : impl_(std::make_unique<impl>(key)) {}
key_file_provider::~key_file_provider() = default;
key_file_provider::key_file_provider(key_file_provider&&) noexcept = default;
key_file_provider& key_file_provider::operator=(key_file_provider&&) noexcept = default;

runtime::task<std::shared_ptr<key_file_provider>> key_file_provider::open(std::filesystem::path path) {
  try {
    if (!std::filesystem::is_regular_file(path)) {
      co_return std::unexpected(key_error(errc::unauthorized, "key file is not a regular file",
                                          path.string()));
    }
#if !defined(_WIN32)
    struct stat status {};
    if (stat(path.c_str(), &status) != 0) {
      co_return std::unexpected(key_error(errc::io, "cannot stat key file", path.string()));
    }
    const auto mode = static_cast<unsigned>(status.st_mode & 0777U);
    if (mode != 0600U) {
      std::ostringstream context;
      context << path.string() << " mode " << std::oct << mode;
      co_return std::unexpected(key_error(errc::unauthorized, "key file must have mode 0600",
                                          context.str()));
    }
#endif
    std::ifstream input(path, std::ios::binary);
    std::array<std::byte, 32> key{};
    input.read(reinterpret_cast<char*>(key.data()), static_cast<std::streamsize>(key.size()));
    if (input.gcount() != static_cast<std::streamsize>(key.size()) || input.peek() != EOF) {
      co_return std::unexpected(key_error(errc::invalid_argument,
                                          "key file must contain exactly 32 bytes", path.string()));
    }
    co_return std::shared_ptr<key_file_provider>(new key_file_provider(key));
  } catch (const std::exception& caught) {
    co_return std::unexpected(key_error(errc::io, caught.what(), path.string()));
  }
}

result<std::array<std::byte, 32>> key_file_provider::derive_kek(
    std::span<const std::byte>, argon2_parameters) const {
  return impl_->key;
}

struct keyring::impl {
  std::map<std::string, std::shared_ptr<key_provider>, std::less<>> providers;
  std::map<std::string, std::array<std::byte, 32>, std::less<>> data_keys;
  std::string default_key;
  std::filesystem::path keys_path;
};

keyring::keyring() : impl_(std::make_unique<impl>()) {}
keyring::~keyring() = default;
keyring::keyring(keyring&&) noexcept = default;
keyring& keyring::operator=(keyring&&) noexcept = default;

result<void> keyring::add(std::string key_id, std::shared_ptr<key_provider> provider) {
  if (!valid_key_id(key_id))
    return std::unexpected(key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
  if (!provider)
    return std::unexpected(key_error(errc::invalid_argument, "key provider is null", key_id));
  impl_->providers[std::move(key_id)] = std::move(provider);
  return {};
}

result<std::string> keyring::default_key_id() const {
  if (impl_->default_key.empty())
    return std::unexpected(key_error(errc::invalid_argument, "no default key is configured", "keyring"));
  return impl_->default_key;
}

result<void> keyring::set_default_key_id(std::string key_id) {
  if (!impl_->providers.contains(key_id))
    return std::unexpected(key_error(errc::unauthorized, "key id is not in the keyring", std::move(key_id)));
  impl_->default_key = std::move(key_id);
  return {};
}

runtime::task<void> keyring::attach(std::filesystem::path keys_path) {
  try {
    std::filesystem::create_directories(keys_path);
    impl_->keys_path = std::move(keys_path);
    co_return result<void>{};
  } catch (const std::exception& caught) {
    co_return std::unexpected(key_error(errc::io, caught.what(), keys_path.string()));
  }
}

runtime::task<std::array<std::byte, 32>> keyring::data_key(std::string key_id,
                                                            bool create_if_missing) {
  const auto provider = impl_->providers.find(key_id);
  if (provider == impl_->providers.end()) {
    co_return std::unexpected(key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
  }
  if (impl_->keys_path.empty()) {
    co_return std::unexpected(key_error(errc::invalid_argument, "keyring is not attached to a store",
                                        key_id));
  }
  if (const auto cached = impl_->data_keys.find(key_id); cached != impl_->data_keys.end())
    co_return cached->second;
  const auto path = impl_->keys_path / (key_id + ".wrapped");
  if (!std::filesystem::exists(path)) {
    if (!create_if_missing) {
      co_return std::unexpected(key_error(errc::unauthorized, "wrapped data key is unavailable", key_id));
    }
    auto data = crypto_detail::random_key();
    if (!data)
      co_return std::unexpected(data.error());
    wrapped_data_key wrapped;
    auto random = crypto_detail::random_bytes(wrapped.salt);
    if (!random)
      co_return std::unexpected(random.error());
    auto kek = provider->second->derive_kek(wrapped.salt, wrapped.parameters);
    if (!kek)
      co_return std::unexpected(kek.error());
    auto wrapping_key = crypto_detail::derive_key(*kek, key_wrap_context);
    if (!wrapping_key)
      co_return std::unexpected(wrapping_key.error());
    auto boxed = crypto_detail::seal(*wrapping_key, {}, *data);
    crypto_wipe(kek->data(), kek->size());
    crypto_wipe(wrapping_key->data(), wrapping_key->size());
    if (!boxed)
      co_return std::unexpected(boxed.error());
    wrapped.boxed = std::move(*boxed);
    auto written = co_await write_atomic(path, encode_wrapped_data_key(wrapped));
    if (!written)
      co_return std::unexpected(written.error());
    impl_->data_keys[key_id] = *data;
    co_return *data;
  }
  auto raw = co_await store_detail::read_file(path);
  if (!raw)
    co_return std::unexpected(raw.error());
  auto wrapped = parse_wrapped_data_key(*raw, key_id);
  if (!wrapped)
    co_return std::unexpected(wrapped.error());
  auto kek = provider->second->derive_kek(wrapped->salt, wrapped->parameters);
  if (!kek)
    co_return std::unexpected(kek.error());
  auto wrapping_key = crypto_detail::derive_key(*kek, key_wrap_context);
  crypto_wipe(kek->data(), kek->size());
  if (!wrapping_key)
    co_return std::unexpected(wrapping_key.error());
  auto data = crypto_detail::open(*wrapping_key, {}, wrapped->boxed);
  crypto_wipe(wrapping_key->data(), wrapping_key->size());
  if (!data || data->size() != 32) {
    co_return std::unexpected(key_error(errc::unauthorized, "cannot unwrap data key", key_id));
  }
  std::array<std::byte, 32> output{};
  std::copy_n(data->begin(), output.size(), output.begin());
  crypto_wipe(data->data(), data->size());
  impl_->data_keys[key_id] = output;
  co_return output;
}

runtime::task<void> keyring::rewrap(std::string key_id,
                                    std::shared_ptr<key_provider> new_provider) {
  if (!new_provider) {
    co_return std::unexpected(key_error(errc::invalid_argument, "key provider is null", key_id));
  }
  auto data = co_await data_key(key_id, false);
  if (!data)
    co_return std::unexpected(data.error());
  const auto path = impl_->keys_path / (key_id + ".wrapped");
  auto raw = co_await store_detail::read_file(path);
  if (!raw)
    co_return std::unexpected(raw.error());
  auto wrapped = parse_wrapped_data_key(*raw, key_id);
  if (!wrapped)
    co_return std::unexpected(wrapped.error());
  auto kek = new_provider->derive_kek(wrapped->salt, wrapped->parameters);
  if (!kek)
    co_return std::unexpected(kek.error());
  auto wrapping_key = crypto_detail::derive_key(*kek, key_wrap_context);
  crypto_wipe(kek->data(), kek->size());
  if (!wrapping_key)
    co_return std::unexpected(wrapping_key.error());
  auto boxed = crypto_detail::seal(*wrapping_key, {}, *data);
  crypto_wipe(wrapping_key->data(), wrapping_key->size());
  crypto_wipe(data->data(), data->size());
  if (!boxed)
    co_return std::unexpected(boxed.error());
  wrapped->boxed = std::move(*boxed);
  auto written = co_await write_atomic(path, encode_wrapped_data_key(*wrapped));
  if (!written)
    co_return std::unexpected(written.error());
  impl_->providers[std::move(key_id)] = std::move(new_provider);
  co_return result<void>{};
}

}  // namespace liquid
