#include "liquid/identity.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
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
#include "identity/taap.hpp"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "store/pack.hpp"

namespace liquid {
namespace {

constexpr std::array<std::byte, 8> key_magic{std::byte{'L'},
                                             std::byte{'Q'},
                                             std::byte{'K'},
                                             std::byte{'E'},
                                             std::byte{'Y'},
                                             std::byte{1},
                                             std::byte{0},
                                             std::byte{0}};
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
        return std::unexpected(
            key_error(errc::invalid_argument, "invalid Argon2i parameters", "passphrase_provider"));
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
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped data key", std::string(key_id)));
    }
    wrapped_data_key output;
    std::copy_n(bytes.begin() + key_magic.size(), output.salt.size(), output.salt.begin());
    output.parameters = {get_u32(bytes, 24), get_u32(bytes, 28), get_u32(bytes, 32)};
    auto valid = validate_argon2(output.parameters);
    if (!valid)
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped data-key parameters", std::string(key_id)));
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

} // namespace

auto identity_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto liquid_home() -> std::filesystem::path {
    if (const auto* configured = std::getenv("LIQUID_HOME")) {
        return configured;
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".liquid";
    }
    return std::filesystem::current_path() / ".liquid";
}

auto read_bytes(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            identity_error(errc::io, "cannot open identity file", path.string()));
    }
    auto output = std::vector<std::byte>{};
    for (char item; input.get(item);) {
        output.push_back(std::byte{static_cast<unsigned char>(item)});
    }
    if (!input.eof()) {
        return std::unexpected(
            identity_error(errc::io, "cannot read identity file", path.string()));
    }
    return output;
}

auto write_private(const std::filesystem::path& path, std::span<const std::byte> value)
    -> result<void> {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    if (!output) {
        return std::unexpected(
            identity_error(errc::io, "cannot write identity file", path.string()));
    }
    output.write(reinterpret_cast<const char*>(value.data()),
                 static_cast<std::streamsize>(value.size()));
    output.flush();
    if (!output) {
        return std::unexpected(
            identity_error(errc::io, "cannot write identity file", path.string()));
    }
#if !defined(_WIN32)
    std::filesystem::permissions(path,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace);
#endif
    return {};
}

auto identity_wrapping_key(const std::filesystem::path& directory)
    -> result<std::array<std::byte, 32>> {
    const auto path = directory / ".wrapping.key";
    if (std::filesystem::exists(path)) {
        auto saved = read_bytes(path);
        if (!saved || saved->size() != 32) {
            return std::unexpected(identity_error(
                errc::unauthorized, "invalid local identity key provider", path.string()));
        }
        auto output = std::array<std::byte, 32>{};
        std::ranges::copy(*saved, output.begin());
        crypto_wipe(saved->data(), saved->size());
        return output;
    }
    auto output = std::array<std::byte, 32>{};
    auto random = crypto_detail::random_bytes(output);
    if (!random) {
        return std::unexpected(random.error());
    }
    auto written = write_private(path, output);
    if (!written) {
        crypto_wipe(output.data(), output.size());
        return std::unexpected(written.error());
    }
    return output;
}

struct signer::impl {
    actor identity;
    std::string label;
    std::array<std::byte, 64> secret{};
    ~impl() {
        crypto_wipe(secret.data(), secret.size());
    }
};

auto actor::identifier() const noexcept -> std::string_view {
    return taap_id;
}

signer::signer() noexcept = default;
signer::~signer() = default;
signer::signer(signer&&) noexcept = default;
signer& signer::operator=(signer&&) noexcept = default;

runtime::task<signature> signer::sign(std::string artifact_type,
                                      std::vector<std::byte> canonical_body) {
    if (!impl_ || artifact_type.empty()) {
        co_return std::unexpected(identity_error(errc::invalid_argument, "signer has no identity"));
    }
    auto input = identity_detail::signing_input(artifact_type, canonical_body);
    auto bytes = std::vector<std::byte>(64);
    crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(bytes.data()),
                        reinterpret_cast<const std::uint8_t*>(impl_->secret.data()),
                        reinterpret_cast<const std::uint8_t*>(input.data()),
                        input.size());
    crypto_wipe(input.data(), input.size());
    co_return signature{"ed25519",
                        impl_->identity.taap_id,
                        impl_->label,
                        std::move(artifact_type),
                        identity_detail::now(),
                        std::move(bytes)};
}

auto signer::identity() const -> actor {
    return impl_ ? impl_->identity : actor{};
}

runtime::task<signer> signer::open(std::string identifier, keyring&) {
    auto opened = co_await identity_detail::local_signer();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (opened->identity().taap_id != identifier) {
        co_return std::unexpected(
            identity_error(errc::not_found, "identity is not available", identifier));
    }
    co_return std::move(*opened);
}

result<bool> verify(const actor& by,
                    std::string_view artifact_type,
                    std::span<const std::byte> canonical_body,
                    const signature& signed_value) {
    if (signed_value.bytes.empty()) {
        return false;
    }
    if (by.algorithm != "ed25519" || by.public_key.size() != 32 || artifact_type.empty() ||
        signed_value.algorithm != "ed25519" || signed_value.bytes.size() != 64 ||
        signed_value.signing_key_identifier != by.taap_id ||
        signed_value.signature_purpose != artifact_type) {
        return std::unexpected(
            identity_error(errc::unauthorized, "invalid TAAP signature metadata"));
    }
    auto input = identity_detail::signing_input(artifact_type, canonical_body);
    const auto valid =
        crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(signed_value.bytes.data()),
                             reinterpret_cast<const std::uint8_t*>(by.public_key.data()),
                             reinterpret_cast<const std::uint8_t*>(input.data()),
                             input.size()) == 0;
    crypto_wipe(input.data(), input.size());
    if (!valid) {
        return std::unexpected(
            identity_error(errc::unauthorized, "TAAP signature verification failed"));
    }
    return true;
}

result<bool> verify_document(const identity_document& document) {
    if (document.subject.algorithm != "ed25519" || document.subject.public_key.size() != 32 ||
        document.signatures.empty()) {
        return std::unexpected(identity_error(errc::unauthorized, "invalid identity document"));
    }
    auto body = identity_detail::canonical_document(document, false);
    if (!body) {
        return std::unexpected(body.error());
    }
    for (const auto& signed_value : document.signatures) {
        auto valid = verify(document.subject, "identity_document", *body, signed_value);
        if (!valid || !*valid) {
            return valid;
        }
    }
    return true;
}

runtime::task<signer> identity_detail::local_signer() {
    const auto directory = liquid_home() / "identity";
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        co_return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    auto public_key = std::vector<std::byte>{};
    auto fingerprint = std::string{};
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (code) {
            co_return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
        }
        if (entry.path().extension() == ".pub") {
            public_key = read_bytes(entry.path()).value_or(std::vector<std::byte>{});
            fingerprint = entry.path().stem().string();
            break;
        }
    }
    auto label = std::string{"liquid-device"};
    auto created_at = identity_detail::now();
    auto secret = std::array<std::byte, 64>{};
    actor identity;
    if (public_key.size() == 32 && !fingerprint.empty()) {
        identity = {"taap:1:ed25519:" + fingerprint, "ed25519", fingerprint, std::move(public_key)};
        auto wrapping_key = identity_wrapping_key(directory);
        if (!wrapping_key) {
            co_return std::unexpected(wrapping_key.error());
        }
        auto sealed = read_bytes(directory / (fingerprint + ".key"));
        if (!sealed) {
            crypto_wipe(wrapping_key->data(), wrapping_key->size());
            co_return std::unexpected(sealed.error());
        }
        auto opened = crypto_detail::open(*wrapping_key, {}, *sealed);
        crypto_wipe(wrapping_key->data(), wrapping_key->size());
        crypto_wipe(sealed->data(), sealed->size());
        if (!opened || opened->size() != secret.size()) {
            co_return std::unexpected(
                identity_error(errc::unauthorized, "cannot unwrap local identity"));
        }
        std::ranges::copy(*opened, secret.begin());
        crypto_wipe(opened->data(), opened->size());
    } else {
        auto seed = std::array<std::byte, 32>{};
        auto random = crypto_detail::random_bytes(seed);
        if (!random) {
            co_return std::unexpected(random.error());
        }
        auto generated_public = std::array<std::byte, 32>{};
        crypto_ed25519_key_pair(reinterpret_cast<std::uint8_t*>(secret.data()),
                                reinterpret_cast<std::uint8_t*>(generated_public.data()),
                                reinterpret_cast<std::uint8_t*>(seed.data()));
        crypto_wipe(seed.data(), seed.size());
        auto derived = identity_detail::identifier(generated_public, created_at, label);
        if (!derived) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(derived.error());
        }
        identity = std::move(*derived);
        auto wrapping_key = identity_wrapping_key(directory);
        if (!wrapping_key) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(wrapping_key.error());
        }
        auto sealed = crypto_detail::seal(*wrapping_key, {}, secret);
        crypto_wipe(wrapping_key->data(), wrapping_key->size());
        if (!sealed) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(sealed.error());
        }
        auto written = write_private(directory / (identity.fingerprint + ".key"), *sealed);
        crypto_wipe(sealed->data(), sealed->size());
        if (!written) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(written.error());
        }
        written = write_private(directory / (identity.fingerprint + ".pub"), identity.public_key);
        if (!written) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(written.error());
        }
        identity_document document{identity, label, created_at, created_at, std::nullopt, {}};
        auto body = identity_detail::canonical_document(document, false);
        if (!body) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(body.error());
        }
        auto input = identity_detail::signing_input("identity_document", *body);
        auto signed_bytes = std::vector<std::byte>(64);
        crypto_ed25519_sign(reinterpret_cast<std::uint8_t*>(signed_bytes.data()),
                            reinterpret_cast<const std::uint8_t*>(secret.data()),
                            reinterpret_cast<const std::uint8_t*>(input.data()),
                            input.size());
        crypto_wipe(input.data(), input.size());
        document.signatures.push_back({"ed25519",
                                       identity.taap_id,
                                       label,
                                       "identity_document",
                                       identity_detail::now(),
                                       std::move(signed_bytes)});
        auto full_document = identity_detail::canonical_document(document, true);
        if (!full_document) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(full_document.error());
        }
        written =
            write_private(directory / (identity.fingerprint + ".identity.cbor"), *full_document);
        if (!written) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(written.error());
        }
    }
    signer output;
    output.impl_ = std::make_unique<signer::impl>();
    output.impl_->identity = std::move(identity);
    output.impl_->label = std::move(label);
    output.impl_->secret = secret;
    crypto_wipe(secret.data(), secret.size());
    co_return output;
}

struct passphrase_provider::impl {
    explicit impl(std::string value) : passphrase(std::move(value)) {}
    ~impl() {
        crypto_wipe(passphrase.data(), passphrase.size());
    }
    std::string passphrase;
};

passphrase_provider::passphrase_provider(std::string passphrase)
    : impl_(std::make_unique<impl>(std::move(passphrase))) {}
passphrase_provider::~passphrase_provider() = default;
passphrase_provider::passphrase_provider(passphrase_provider&&) noexcept = default;
passphrase_provider& passphrase_provider::operator=(passphrase_provider&&) noexcept = default;

result<std::array<std::byte, 32>> passphrase_provider::derive(std::span<const std::byte> salt,
                                                              argon2_parameters parameters) const {
    auto valid = validate_argon2(parameters);
    if (!valid)
        return std::unexpected(valid.error());
    if (salt.size() < 8 || salt.size() > std::numeric_limits<std::uint32_t>::max() ||
        impl_->passphrase.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(key_error(
            errc::invalid_argument, "invalid passphrase or salt length", "passphrase_provider"));
    }
    std::vector<std::byte> work(static_cast<std::size_t>(parameters.memory_kib) * 1024U);
    std::array<std::byte, 32> output{};
    const crypto_argon2_config config{
        CRYPTO_ARGON2_I, parameters.memory_kib, parameters.iterations, parameters.lanes};
    const crypto_argon2_inputs input{
        reinterpret_cast<const std::uint8_t*>(impl_->passphrase.data()),
        reinterpret_cast<const std::uint8_t*>(salt.data()),
        static_cast<std::uint32_t>(impl_->passphrase.size()),
        static_cast<std::uint32_t>(salt.size())};
    crypto_argon2(reinterpret_cast<std::uint8_t*>(output.data()),
                  output.size(),
                  work.data(),
                  config,
                  input,
                  crypto_argon2_no_extras);
    return output;
}

result<std::array<std::byte, 32>>
passphrase_provider::derive_kek(std::span<const std::byte> salt,
                                argon2_parameters parameters) const {
    return derive(salt, parameters);
}

struct key_file_provider::impl {
    explicit impl(std::array<std::byte, 32> value) : key(value) {}
    ~impl() {
        crypto_wipe(key.data(), key.size());
    }
    std::array<std::byte, 32> key;
};

key_file_provider::key_file_provider(std::array<std::byte, 32> key) noexcept
    : impl_(std::make_unique<impl>(key)) {}
key_file_provider::~key_file_provider() = default;
key_file_provider::key_file_provider(key_file_provider&&) noexcept = default;
key_file_provider& key_file_provider::operator=(key_file_provider&&) noexcept = default;

runtime::task<std::shared_ptr<key_file_provider>>
key_file_provider::open(std::filesystem::path path) {
    try {
        if (!std::filesystem::is_regular_file(path)) {
            co_return std::unexpected(
                key_error(errc::unauthorized, "key file is not a regular file", path.string()));
        }
#if !defined(_WIN32)
        struct stat status{};
        if (stat(path.c_str(), &status) != 0) {
            co_return std::unexpected(key_error(errc::io, "cannot stat key file", path.string()));
        }
        const auto mode = static_cast<unsigned>(status.st_mode & 0777U);
        if (mode != 0600U) {
            std::ostringstream context;
            context << path.string() << " mode " << std::oct << mode;
            co_return std::unexpected(
                key_error(errc::unauthorized, "key file must have mode 0600", context.str()));
        }
#endif
        std::ifstream input(path, std::ios::binary);
        std::array<std::byte, 32> key{};
        input.read(reinterpret_cast<char*>(key.data()), static_cast<std::streamsize>(key.size()));
        if (input.gcount() != static_cast<std::streamsize>(key.size()) || input.peek() != EOF) {
            co_return std::unexpected(key_error(
                errc::invalid_argument, "key file must contain exactly 32 bytes", path.string()));
        }
        co_return std::shared_ptr<key_file_provider>(new key_file_provider(key));
    } catch (const std::exception& caught) {
        co_return std::unexpected(key_error(errc::io, caught.what(), path.string()));
    }
}

result<std::array<std::byte, 32>> key_file_provider::derive_kek(std::span<const std::byte>,
                                                                argon2_parameters) const {
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
        return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    if (!provider)
        return std::unexpected(key_error(errc::invalid_argument, "key provider is null", key_id));
    impl_->providers[std::move(key_id)] = std::move(provider);
    return {};
}

result<std::string> keyring::default_key_id() const {
    if (impl_->default_key.empty())
        return std::unexpected(
            key_error(errc::invalid_argument, "no default key is configured", "keyring"));
    return impl_->default_key;
}

result<void> keyring::set_default_key_id(std::string key_id) {
    if (!impl_->providers.contains(key_id))
        return std::unexpected(
            key_error(errc::unauthorized, "key id is not in the keyring", std::move(key_id)));
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
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    if (const auto cached = impl_->data_keys.find(key_id); cached != impl_->data_keys.end())
        co_return cached->second;
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    if (!std::filesystem::exists(path)) {
        if (!create_if_missing) {
            co_return std::unexpected(
                key_error(errc::unauthorized, "wrapped data key is unavailable", key_id));
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
        co_return std::unexpected(
            key_error(errc::invalid_argument, "key provider is null", key_id));
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

} // namespace liquid
