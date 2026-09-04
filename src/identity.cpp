#include "liquid/identity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <set>
#include <sstream>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <sys/stat.h>
#endif

#include "crypto/crypto.hpp"
#include "identity/key_version.hpp"
#include "identity/taap.hpp"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "store/pack.hpp"

namespace liquid {
namespace {

constexpr std::array<std::byte, 8> key_magic_v1{std::byte{'L'},
                                                std::byte{'Q'},
                                                std::byte{'K'},
                                                std::byte{'E'},
                                                std::byte{'Y'},
                                                std::byte{1},
                                                std::byte{0},
                                                std::byte{0}};
constexpr std::array<std::byte, 8> key_magic_v2{std::byte{'L'},
                                                std::byte{'Q'},
                                                std::byte{'K'},
                                                std::byte{'E'},
                                                std::byte{'Y'},
                                                std::byte{2},
                                                std::byte{0},
                                                std::byte{0}};
constexpr std::string_view key_wrap_context = "liquid store data key wrapping v1";
constexpr std::size_t boxed_key_size = 24 + 16 + 32;

error key_error(errc code, std::string message, std::string context) {
    return {code, std::move(message), std::move(context)};
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
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

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 8 > bytes.size())
        return 0;
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + index])) << (index * 8U);
    return value;
}

bool valid_key_id(std::string_view key_id) {
    return !key_id.empty() && key_id != "." && key_id != ".." &&
           key_id.size() <= std::numeric_limits<std::uint16_t>::max() &&
           key_id.find_first_of("/\\@") == std::string_view::npos &&
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

struct wrapped_key_file {
    std::array<std::byte, 16> salt{};
    argon2_parameters parameters{};
    std::uint64_t current_version{1};
    std::map<std::uint64_t, std::vector<std::byte>> versions;
    std::set<std::uint64_t> revoked;
};

result<wrapped_key_file> parse_wrapped_key_file(std::span<const std::byte> bytes,
                                                std::string_view key_id) {
    constexpr std::size_t legacy_header_size = key_magic_v1.size() + 16 + 12;
    const bool legacy = bytes.size() == legacy_header_size + boxed_key_size &&
                        std::equal(key_magic_v1.begin(), key_magic_v1.end(), bytes.begin());
    const bool current =
        bytes.size() >= 52 && std::equal(key_magic_v2.begin(), key_magic_v2.end(), bytes.begin());
    if (!legacy && !current) {
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped data key", std::string(key_id)));
    }
    wrapped_key_file output;
    std::copy_n(bytes.begin() + key_magic_v1.size(), output.salt.size(), output.salt.begin());
    output.parameters = {get_u32(bytes, 24), get_u32(bytes, 28), get_u32(bytes, 32)};
    auto valid = validate_argon2(output.parameters);
    if (!valid)
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped data-key parameters", std::string(key_id)));
    if (legacy) {
        output.versions.emplace(
            1, std::vector<std::byte>{bytes.begin() + legacy_header_size, bytes.end()});
        return output;
    }
    output.current_version = get_u64(bytes, 36);
    const auto version_count = get_u32(bytes, 44);
    const auto revoked_count = get_u32(bytes, 48);
    const auto expected = 52U + static_cast<std::size_t>(version_count) * (8U + boxed_key_size) +
                          static_cast<std::size_t>(revoked_count) * 8U;
    if (output.current_version == 0 || expected != bytes.size()) {
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped key-version table", std::string(key_id)));
    }
    std::size_t at = 52;
    for (std::uint32_t index = 0; index != version_count; ++index) {
        const auto version = get_u64(bytes, at);
        at += 8;
        if (version == 0 || output.versions.contains(version)) {
            return std::unexpected(
                key_error(errc::corrupt, "invalid wrapped key version", std::string(key_id)));
        }
        output.versions.emplace(
            version,
            std::vector<std::byte>{bytes.begin() + at, bytes.begin() + at + boxed_key_size});
        at += boxed_key_size;
    }
    for (std::uint32_t index = 0; index != revoked_count; ++index) {
        const auto version = get_u64(bytes, at);
        at += 8;
        if (version == 0 || output.versions.contains(version) ||
            !output.revoked.insert(version).second) {
            return std::unexpected(
                key_error(errc::corrupt, "invalid revoked key version", std::string(key_id)));
        }
    }
    if (!output.versions.contains(output.current_version) &&
        !output.revoked.contains(output.current_version)) {
        return std::unexpected(
            key_error(errc::corrupt, "current key version is missing", std::string(key_id)));
    }
    return output;
}

std::vector<std::byte> encode_wrapped_key_file(const wrapped_key_file& wrapped) {
    std::vector<std::byte> output(key_magic_v2.begin(), key_magic_v2.end());
    output.insert(output.end(), wrapped.salt.begin(), wrapped.salt.end());
    put_u32(output, wrapped.parameters.memory_kib);
    put_u32(output, wrapped.parameters.iterations);
    put_u32(output, wrapped.parameters.lanes);
    put_u64(output, wrapped.current_version);
    put_u32(output, static_cast<std::uint32_t>(wrapped.versions.size()));
    put_u32(output, static_cast<std::uint32_t>(wrapped.revoked.size()));
    for (const auto& [version, boxed] : wrapped.versions) {
        put_u64(output, version);
        output.insert(output.end(), boxed.begin(), boxed.end());
    }
    for (const auto version : wrapped.revoked)
        put_u64(output, version);
    return output;
}

std::vector<std::string> encryption_limits() {
    return {"Rotation does not rewrite history.",
            "Revoking a collaborator prevents future access; it does not recall what they have.",
            "Metadata — paths, sizes, change structure and timing — remains visible unless "
            "separately protected."};
}

runtime::task<void> write_atomic(std::filesystem::path path, std::vector<std::byte> bytes) {
    const auto temporary = path.string() + ".tmp";
    auto written = co_await store_detail::write_file(temporary, std::move(bytes));
    if (!written)
        co_return std::unexpected(written.error());
    try {
#if defined(_WIN32)
        if (!MoveFileExW(std::filesystem::path{temporary}.c_str(),
                         path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto code =
                std::error_code{static_cast<int>(GetLastError()), std::system_category()};
            co_return std::unexpected(key_error(errc::io, code.message(), path.string()));
        }
#else
        std::filesystem::rename(temporary, path);
#endif
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

auto signing_key_identifier(const actor& value) -> std::string {
    const auto& fingerprint =
        value.key_fingerprint.empty() ? value.fingerprint : value.key_fingerprint;
    return "taap:1:ed25519:" + fingerprint;
}

auto valid_fingerprint(std::string_view value) -> bool {
    if (value.size() != 52) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '2' && character <= '7');
    });
}

auto valid_timestamp(std::string_view value) -> bool {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }
    for (const auto index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U}) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    const auto number = [&](std::size_t offset, std::size_t count) {
        return std::stoi(std::string{value.substr(offset, count)});
    };
    const auto day = std::chrono::year{number(0, 4)} /
                     std::chrono::month{static_cast<unsigned>(number(5, 2))} /
                     std::chrono::day{static_cast<unsigned>(number(8, 2))};
    return day.ok() && number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
}

auto next_timestamp(std::string_view previous) -> result<std::string> {
    if (!valid_timestamp(previous)) {
        return std::unexpected(
            identity_error(errc::invalid_argument, "invalid previous update time", "updated_at"));
    }
    auto current = identity_detail::now();
    if (current > previous) {
        return current;
    }
    const auto number = [&](std::size_t offset, std::size_t count) {
        return std::stoi(std::string{previous.substr(offset, count)});
    };
    const auto date = std::chrono::year{number(0, 4)} /
                      std::chrono::month{static_cast<unsigned>(number(5, 2))} /
                      std::chrono::day{static_cast<unsigned>(number(8, 2))};
    const auto value = std::chrono::sys_days{date} + std::chrono::hours{number(11, 2)} +
                       std::chrono::minutes{number(14, 2)} + std::chrono::seconds{number(17, 2)} +
                       std::chrono::seconds{1};
    return std::format("{:%FT%TZ}", value);
}

auto document_directory(const actor& subject) -> std::filesystem::path {
    return liquid_home() / "identity" / subject.fingerprint / "documents";
}

auto document_path(const std::filesystem::path& directory, std::size_t index)
    -> std::filesystem::path {
    return directory / std::format("{:03}.cbor", index);
}

struct stored_document {
    identity_document document;
    std::vector<std::byte> bytes;
};

auto load_stored_documents(const actor& subject) -> result<std::vector<stored_document>> {
    const auto directory = document_directory(subject);
    auto paths = std::vector<std::filesystem::path>{};
    std::error_code code;
    if (std::filesystem::exists(directory, code)) {
        if (code) {
            return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
            if (code) {
                return std::unexpected(
                    identity_error(errc::io, code.message(), directory.string()));
            }
            if (entry.is_regular_file() && entry.path().extension() == ".cbor") {
                paths.push_back(entry.path());
            }
        }
    }
    if (paths.empty()) {
        const auto legacy = liquid_home() / "identity" / (subject.fingerprint + ".identity.cbor");
        if (std::filesystem::exists(legacy, code) && !code) {
            paths.push_back(legacy);
        }
    }
    std::ranges::sort(paths);
    auto output = std::vector<stored_document>{};
    for (const auto& path : paths) {
        auto bytes = read_bytes(path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto document = identity_detail::parse_document(*bytes);
        if (!document) {
            return std::unexpected(document.error());
        }
        output.push_back({std::move(*document), std::move(*bytes)});
    }
    if (output.empty()) {
        return std::unexpected(identity_error(
            errc::not_found, "identity document chain is unavailable", subject.taap_id));
    }
    return output;
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
                        signing_key_identifier(impl_->identity),
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

namespace {

auto valid_compromise_declaration(const compromise_declaration& declaration) -> bool;

} // namespace

runtime::task<identity_document> signer::rotate(std::optional<compromise_declaration> compromise) {
    if (!impl_) {
        co_return std::unexpected(identity_error(errc::invalid_argument, "signer has no identity"));
    }
    auto stored = load_stored_documents(impl_->identity);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto documents = std::vector<identity_document>{};
    documents.reserve(stored->size());
    for (const auto& document : *stored) {
        documents.push_back(document.document);
    }
    auto verified = verify_chain(documents);
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    const auto& current = stored->back().document;
    if (current.subject.taap_id != impl_->identity.taap_id ||
        current.subject.key_fingerprint != impl_->identity.key_fingerprint) {
        co_return std::unexpected(identity_error(
            errc::unauthorized, "signer is not the current identity key", impl_->identity.taap_id));
    }
    if (compromise && compromise->key_fingerprint_base32 != current.subject.key_fingerprint) {
        co_return std::unexpected(identity_error(errc::invalid_argument,
                                                 "compromise declaration must name the current key",
                                                 impl_->identity.taap_id));
    }
    if (compromise && !valid_compromise_declaration(*compromise)) {
        co_return std::unexpected(identity_error(
            errc::invalid_argument, "invalid compromise declaration", impl_->identity.taap_id));
    }
    auto updated_at = next_timestamp(current.updated_at);
    if (!updated_at) {
        co_return std::unexpected(updated_at.error());
    }
    const auto label = impl_->label + "-rotated-" + std::to_string(stored->size());
    auto seed = std::array<std::byte, 32>{};
    auto random = crypto_detail::random_bytes(seed);
    if (!random) {
        co_return std::unexpected(random.error());
    }
    auto new_secret = std::array<std::byte, 64>{};
    auto public_key = std::array<std::byte, 32>{};
    crypto_ed25519_key_pair(reinterpret_cast<std::uint8_t*>(new_secret.data()),
                            reinterpret_cast<std::uint8_t*>(public_key.data()),
                            reinterpret_cast<std::uint8_t*>(seed.data()));
    crypto_wipe(seed.data(), seed.size());
    auto new_identity = identity_detail::identifier(public_key, *updated_at, label);
    if (!new_identity) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(new_identity.error());
    }
    new_identity->taap_id = current.subject.taap_id;
    new_identity->fingerprint = current.subject.fingerprint;
    const auto predecessor_hash = identity_detail::sha512_256(stored->back().bytes);
    identity_document successor{
        std::move(*new_identity),
        label,
        *updated_at,
        *updated_at,
        std::vector<std::byte>{predecessor_hash.begin(), predecessor_hash.end()},
        {},
        {}};
    if (compromise) {
        successor.compromised_keys.push_back(std::move(*compromise));
    }
    auto body = identity_detail::canonical_document(successor, false);
    if (!body) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(body.error());
    }
    auto directory = liquid_home() / "identity";
    std::error_code code;
    std::filesystem::create_directories(document_directory(current.subject), code);
    if (code) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    for (std::size_t index = 0; index < stored->size(); ++index) {
        const auto path = document_path(document_directory(current.subject), index);
        if (std::filesystem::exists(path, code)) {
            if (code) {
                crypto_wipe(new_secret.data(), new_secret.size());
                co_return std::unexpected(identity_error(errc::io, code.message(), path.string()));
            }
            continue;
        }
        auto copied = write_private(path, (*stored)[index].bytes);
        if (!copied) {
            crypto_wipe(new_secret.data(), new_secret.size());
            co_return std::unexpected(copied.error());
        }
    }
    auto wrapping_key = identity_wrapping_key(directory);
    if (!wrapping_key) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(wrapping_key.error());
    }
    auto sealed = crypto_detail::seal(*wrapping_key, {}, new_secret);
    crypto_wipe(wrapping_key->data(), wrapping_key->size());
    if (!sealed) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(sealed.error());
    }
    if (successor.compromised_keys.empty()) {
        auto old_signature = co_await sign("identity_document", *body);
        if (!old_signature) {
            crypto_wipe(new_secret.data(), new_secret.size());
            crypto_wipe(sealed->data(), sealed->size());
            co_return std::unexpected(old_signature.error());
        }
        successor.signatures.push_back(std::move(*old_signature));
    }
    signer next;
    next.impl_ = std::make_unique<signer::impl>();
    next.impl_->identity = successor.subject;
    next.impl_->label = successor.key_label;
    next.impl_->secret = new_secret;
    auto new_signature = co_await next.sign("identity_document", *body);
    if (!new_signature) {
        crypto_wipe(new_secret.data(), new_secret.size());
        crypto_wipe(sealed->data(), sealed->size());
        co_return std::unexpected(new_signature.error());
    }
    successor.signatures.push_back(std::move(*new_signature));
    auto encoded = identity_detail::canonical_document(successor, true);
    if (!encoded) {
        crypto_wipe(new_secret.data(), new_secret.size());
        crypto_wipe(sealed->data(), sealed->size());
        co_return std::unexpected(encoded.error());
    }
    crypto_wipe(impl_->secret.data(), impl_->secret.size());
    auto written = write_private(directory / (current.subject.fingerprint + ".key"), *sealed);
    crypto_wipe(sealed->data(), sealed->size());
    if (!written) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(written.error());
    }
    written = write_private(directory / (current.subject.fingerprint + ".pub"),
                            successor.subject.public_key);
    if (!written) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(written.error());
    }
    written =
        write_private(document_path(document_directory(current.subject), stored->size()), *encoded);
    if (!written) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(written.error());
    }
    written = write_private(directory / (current.subject.fingerprint + ".identity.cbor"), *encoded);
    if (!written) {
        crypto_wipe(new_secret.data(), new_secret.size());
        co_return std::unexpected(written.error());
    }
    impl_->identity = successor.subject;
    impl_->label = successor.key_label;
    impl_->secret = new_secret;
    crypto_wipe(new_secret.data(), new_secret.size());
    co_return successor;
}

namespace {

auto verify_with_key(const actor& by,
                     std::string_view artifact_type,
                     std::span<const std::byte> canonical_body,
                     const signature& signed_value) -> result<bool> {
    if (signed_value.bytes.empty()) {
        return false;
    }
    if (by.algorithm != "ed25519" || by.public_key.size() != 32 || artifact_type.empty() ||
        signed_value.algorithm != "ed25519" || signed_value.bytes.size() != 64 ||
        signed_value.signing_key_identifier != signing_key_identifier(by) ||
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

auto chain_failure(std::string name) -> error {
    return identity_error(errc::unauthorized, std::move(name), "identity chain");
}

auto valid_compromise_declaration(const compromise_declaration& declaration) -> bool {
    return valid_fingerprint(declaration.key_fingerprint_base32) &&
           valid_timestamp(declaration.declared_at);
}

auto valid_subject(const identity_document& document) -> bool {
    if (document.subject.algorithm != "ed25519" || document.subject.public_key.size() != 32 ||
        !valid_fingerprint(document.subject.fingerprint) ||
        !valid_fingerprint(document.subject.key_fingerprint) ||
        document.subject.taap_id != "taap:1:ed25519:" + document.subject.fingerprint) {
        return false;
    }
    auto expected = identity_detail::key_fingerprint(
        document.subject.public_key, document.created_at, document.key_label);
    return expected && *expected == document.subject.key_fingerprint;
}

auto declares_compromised(const identity_document& document, std::string_view fingerprint)
    -> const compromise_declaration* {
    for (const auto& declaration : document.compromised_keys) {
        if (declaration.key_fingerprint_base32 == fingerprint) {
            return &declaration;
        }
    }
    return nullptr;
}

} // namespace

result<bool> verify(const actor& by,
                    std::string_view artifact_type,
                    std::span<const std::byte> canonical_body,
                    const signature& signed_value) {
    auto stored = load_stored_documents(by);
    if (!stored) {
        if (stored.error().code == errc::not_found) {
            return verify_with_key(by, artifact_type, canonical_body, signed_value);
        }
        return std::unexpected(stored.error());
    }
    auto documents = std::vector<identity_document>{};
    documents.reserve(stored->size());
    for (const auto& document : *stored) {
        documents.push_back(document.document);
    }
    auto chain = verify_chain(documents);
    if (!chain) {
        return std::unexpected(chain.error());
    }
    for (std::size_t index = 0; index < documents.size(); ++index) {
        const auto& candidate = documents[index].subject;
        if (signed_value.signing_key_identifier != signing_key_identifier(candidate)) {
            continue;
        }
        for (std::size_t successor = index + 1; successor < documents.size(); ++successor) {
            const auto* declaration =
                declares_compromised(documents[successor], candidate.key_fingerprint);
            if (declaration) {
                if (!valid_timestamp(signed_value.signed_at) ||
                    signed_value.signed_at >= declaration->declared_at) {
                    return std::unexpected(identity_error(errc::unauthorized,
                                                          "signature was made after key compromise",
                                                          candidate.taap_id));
                }
            }
        }
        return verify_with_key(candidate, artifact_type, canonical_body, signed_value);
    }
    return std::unexpected(
        identity_error(errc::unauthorized, "signing key is not in the identity chain", by.taap_id));
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
        if (signed_value.signing_key_identifier != signing_key_identifier(document.subject)) {
            continue;
        }
        auto valid = verify_with_key(document.subject, "identity_document", *body, signed_value);
        if (valid && *valid) {
            return true;
        }
        return valid;
    }
    return std::unexpected(identity_error(
        errc::unauthorized, "identity document lacks a self-signature", document.subject.taap_id));
}

result<verified_identity> verify_chain(std::span<const identity_document> documents) {
    if (documents.empty()) {
        return std::unexpected(chain_failure("identity_chain_empty"));
    }
    const auto& first = documents.front();
    if (first.previous_document_hash) {
        return std::unexpected(chain_failure("identity_chain_unexpected_predecessor"));
    }
    if (!valid_timestamp(first.created_at) || !valid_timestamp(first.updated_at) ||
        first.updated_at < first.created_at || !valid_subject(first)) {
        return std::unexpected(chain_failure("identity_chain_invalid_document"));
    }
    auto self_signed = verify_document(first);
    if (!self_signed || !*self_signed) {
        return std::unexpected(chain_failure("identity_chain_invalid_self_signature"));
    }
    auto compromised_history = false;
    for (const auto& declaration : first.compromised_keys) {
        if (!valid_compromise_declaration(declaration)) {
            return std::unexpected(chain_failure("identity_chain_invalid_compromise_declaration"));
        }
        compromised_history = true;
    }
    for (std::size_t index = 1; index < documents.size(); ++index) {
        const auto& previous = documents[index - 1];
        const auto& current = documents[index];
        if (current.subject.taap_id != first.subject.taap_id ||
            current.subject.fingerprint != first.subject.fingerprint || !valid_subject(current)) {
            return std::unexpected(chain_failure("identity_chain_identifier_changed"));
        }
        if (!valid_timestamp(current.created_at) || !valid_timestamp(current.updated_at) ||
            current.updated_at <= previous.updated_at) {
            return std::unexpected(chain_failure("identity_chain_non_monotonic_updated_at"));
        }
        auto previous_bytes = identity_detail::canonical_document(previous, true);
        if (!previous_bytes) {
            return std::unexpected(previous_bytes.error());
        }
        const auto expected_hash = identity_detail::sha512_256(*previous_bytes);
        if (!current.previous_document_hash ||
            !std::ranges::equal(*current.previous_document_hash, expected_hash)) {
            return std::unexpected(chain_failure("identity_chain_broken_predecessor_hash"));
        }
        for (const auto& declaration : current.compromised_keys) {
            if (!valid_compromise_declaration(declaration)) {
                return std::unexpected(
                    chain_failure("identity_chain_invalid_compromise_declaration"));
            }
            compromised_history = true;
        }
        auto body = identity_detail::canonical_document(current, false);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto new_key_signature = false;
        auto old_key_signature = false;
        for (const auto& signed_value : current.signatures) {
            if (signed_value.signing_key_identifier == signing_key_identifier(current.subject)) {
                auto valid =
                    verify_with_key(current.subject, "identity_document", *body, signed_value);
                if (!valid || !*valid) {
                    return std::unexpected(
                        chain_failure("identity_chain_invalid_new_key_signature"));
                }
                new_key_signature = true;
            }
            if (signed_value.signing_key_identifier == signing_key_identifier(previous.subject)) {
                auto valid =
                    verify_with_key(previous.subject, "identity_document", *body, signed_value);
                if (!valid || !*valid) {
                    return std::unexpected(
                        chain_failure("identity_chain_invalid_old_key_signature"));
                }
                old_key_signature = true;
            }
        }
        if (!new_key_signature) {
            return std::unexpected(chain_failure("identity_chain_missing_new_key_signature"));
        }
        if (!declares_compromised(current, previous.subject.key_fingerprint) &&
            !old_key_signature) {
            return std::unexpected(chain_failure("identity_chain_missing_dual_signature"));
        }
    }
    return verified_identity{
        documents.back().subject, {documents.back().subject}, compromised_history};
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
        auto provisional =
            actor{"taap:1:ed25519:" + fingerprint, "ed25519", fingerprint, public_key, {}};
        auto stored = load_stored_documents(provisional);
        if (!stored) {
            co_return std::unexpected(stored.error());
        }
        auto documents = std::vector<identity_document>{};
        documents.reserve(stored->size());
        for (const auto& document : *stored) {
            documents.push_back(document.document);
        }
        auto verified = verify_chain(documents);
        if (!verified) {
            co_return std::unexpected(verified.error());
        }
        identity = stored->back().document.subject;
        label = stored->back().document.key_label;
        if (identity.public_key != public_key) {
            co_return std::unexpected(
                identity_error(errc::corrupt,
                               "current identity document does not match public key",
                               identity.taap_id));
        }
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
        identity_document document{identity, label, created_at, created_at, std::nullopt, {}, {}};
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
                                       signing_key_identifier(identity),
                                       label,
                                       "identity_document",
                                       identity_detail::now(),
                                       std::move(signed_bytes)});
        auto full_document = identity_detail::canonical_document(document, true);
        if (!full_document) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(full_document.error());
        }
        std::filesystem::create_directories(document_directory(identity), code);
        if (code) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
        }
        written = write_private(document_path(document_directory(identity), 0), *full_document);
        if (!written) {
            crypto_wipe(secret.data(), secret.size());
            co_return std::unexpected(written.error());
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

namespace {

result<crypto_detail::key> derive_wrapping_key(result<crypto_detail::key> kek) {
    if (!kek)
        return std::unexpected(kek.error());
    auto wrapping = crypto_detail::derive_key(*kek, key_wrap_context);
    crypto_detail::wipe(*kek);
    return wrapping;
}

result<crypto_detail::key> unwrap_key(const crypto_detail::key& wrapping,
                                      std::span<const std::byte> boxed,
                                      std::string_view reference) {
    auto opened = crypto_detail::open(wrapping, {}, boxed);
    if (!opened || opened->size() != crypto_detail::key{}.size()) {
        if (opened)
            crypto_detail::wipe(*opened);
        return std::unexpected(
            key_error(errc::unauthorized, "cannot unwrap data key", std::string{reference}));
    }
    crypto_detail::key output{};
    std::copy_n(opened->begin(), output.size(), output.begin());
    crypto_detail::wipe(*opened);
    return output;
}

} // namespace

struct keyring::impl {
    std::map<std::string, std::shared_ptr<key_provider>, std::less<>> providers;
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

runtime::task<std::string> keyring::write_key(std::string key_id) {
    if (!valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    if (!impl_->providers.contains(key_id)) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    if (!std::filesystem::exists(path)) {
        const auto reference = identity_detail::versioned_key_id(key_id, 1);
        auto data = co_await data_key(reference, true);
        if (!data)
            co_return std::unexpected(data.error());
        crypto_detail::wipe(*data);
        co_return reference;
    }
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    const auto reference = identity_detail::versioned_key_id(key_id, file->current_version);
    if (!file->versions.contains(file->current_version)) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "write key version is unavailable", reference));
    }
    co_return reference;
}

runtime::task<std::array<std::byte, 32>> keyring::data_key(std::string key_id,
                                                           bool create_if_missing) {
    auto parsed = identity_detail::parse_key_reference(key_id);
    if (!parsed)
        co_return std::unexpected(parsed.error());
    if (!valid_key_id(parsed->key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    const auto provider = impl_->providers.find(parsed->key_id);
    if (provider == impl_->providers.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto version = parsed->version.value_or(1);
    const auto reference = identity_detail::versioned_key_id(parsed->key_id, version);
    const auto path = impl_->keys_path / (parsed->key_id + ".wrapped");
    if (!std::filesystem::exists(path)) {
        if (!create_if_missing || version != 1) {
            co_return std::unexpected(
                key_error(errc::unauthorized, "wrapped data key is unavailable", reference));
        }
        auto data = crypto_detail::random_key();
        if (!data)
            co_return std::unexpected(data.error());
        wrapped_key_file file;
        auto random = crypto_detail::random_bytes(file.salt);
        if (!random) {
            crypto_detail::wipe(*data);
            co_return std::unexpected(random.error());
        }
        auto wrapping =
            derive_wrapping_key(provider->second->derive_kek(file.salt, file.parameters));
        if (!wrapping) {
            crypto_detail::wipe(*data);
            co_return std::unexpected(wrapping.error());
        }
        auto boxed = crypto_detail::seal(*wrapping, {}, *data);
        crypto_detail::wipe(*wrapping);
        if (!boxed) {
            crypto_detail::wipe(*data);
            co_return std::unexpected(boxed.error());
        }
        file.versions.emplace(1, std::move(*boxed));
        auto written = co_await write_atomic(path, encode_wrapped_key_file(file));
        if (!written) {
            crypto_detail::wipe(*data);
            co_return std::unexpected(written.error());
        }
        auto output = *data;
        crypto_detail::wipe(*data);
        co_return output;
    }
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, parsed->key_id);
    if (!file)
        co_return std::unexpected(file.error());
    const auto boxed = file->versions.find(version);
    if (boxed == file->versions.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "data key version is unavailable", reference));
    }
    auto wrapping = derive_wrapping_key(provider->second->derive_kek(file->salt, file->parameters));
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto data = unwrap_key(*wrapping, boxed->second, reference);
    crypto_detail::wipe(*wrapping);
    if (!data)
        co_return std::unexpected(data.error());
    auto output = *data;
    crypto_detail::wipe(*data);
    co_return output;
}

runtime::task<void> keyring::rewrap(std::string key_id,
                                    std::shared_ptr<key_provider> new_provider) {
    if (!new_provider) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "key provider is null", key_id));
    }
    if (!valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    const auto provider = impl_->providers.find(key_id);
    if (provider == impl_->providers.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    if (file->versions.empty()) {
        co_return std::unexpected(key_error(
            errc::unauthorized, "no held data key version can authenticate the KEK", key_id));
    }
    auto old_wrapping =
        derive_wrapping_key(provider->second->derive_kek(file->salt, file->parameters));
    if (!old_wrapping)
        co_return std::unexpected(old_wrapping.error());
    auto new_wrapping = derive_wrapping_key(new_provider->derive_kek(file->salt, file->parameters));
    if (!new_wrapping) {
        crypto_detail::wipe(*old_wrapping);
        co_return std::unexpected(new_wrapping.error());
    }
    auto replacement = *file;
    replacement.versions.clear();
    for (const auto& [version, boxed] : file->versions) {
        const auto reference = identity_detail::versioned_key_id(key_id, version);
        auto data = unwrap_key(*old_wrapping, boxed, reference);
        if (!data) {
            crypto_detail::wipe(*old_wrapping);
            crypto_detail::wipe(*new_wrapping);
            co_return std::unexpected(data.error());
        }
        auto reboxed = crypto_detail::seal(*new_wrapping, {}, *data);
        crypto_detail::wipe(*data);
        if (!reboxed) {
            crypto_detail::wipe(*old_wrapping);
            crypto_detail::wipe(*new_wrapping);
            co_return std::unexpected(reboxed.error());
        }
        replacement.versions.emplace(version, std::move(*reboxed));
    }
    crypto_detail::wipe(*old_wrapping);
    crypto_detail::wipe(*new_wrapping);
    auto written = co_await write_atomic(path, encode_wrapped_key_file(replacement));
    if (!written)
        co_return std::unexpected(written.error());
    impl_->providers[std::move(key_id)] = std::move(new_provider);
    co_return result<void>{};
}

runtime::task<rotation_report> keyring::rotate(std::string key_id) {
    if (!valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    const auto provider = impl_->providers.find(key_id);
    if (provider == impl_->providers.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    if (file->versions.empty()) {
        co_return std::unexpected(key_error(
            errc::unauthorized, "no held data key version can authenticate the KEK", key_id));
    }
    auto wrapping = derive_wrapping_key(provider->second->derive_kek(file->salt, file->parameters));
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    const auto authentication = file->versions.contains(file->current_version)
                                    ? file->versions.find(file->current_version)
                                    : file->versions.begin();
    const auto authentication_reference =
        identity_detail::versioned_key_id(key_id, authentication->first);
    auto authenticated = unwrap_key(*wrapping, authentication->second, authentication_reference);
    if (!authenticated) {
        crypto_detail::wipe(*wrapping);
        co_return std::unexpected(authenticated.error());
    }
    crypto_detail::wipe(*authenticated);
    auto highest = file->current_version;
    if (!file->versions.empty())
        highest = std::max(highest, file->versions.rbegin()->first);
    if (!file->revoked.empty())
        highest = std::max(highest, *file->revoked.rbegin());
    if (highest == std::numeric_limits<std::uint64_t>::max()) {
        crypto_detail::wipe(*wrapping);
        co_return std::unexpected(
            key_error(errc::invalid_argument, "key version space is exhausted", key_id));
    }
    auto data = crypto_detail::random_key();
    if (!data) {
        crypto_detail::wipe(*wrapping);
        co_return std::unexpected(data.error());
    }
    auto boxed = crypto_detail::seal(*wrapping, {}, *data);
    crypto_detail::wipe(*data);
    crypto_detail::wipe(*wrapping);
    if (!boxed)
        co_return std::unexpected(boxed.error());
    const auto previous = file->current_version;
    const auto next = highest + 1;
    file->current_version = next;
    file->versions.emplace(next, std::move(*boxed));
    auto written = co_await write_atomic(path, encode_wrapped_key_file(*file));
    if (!written)
        co_return std::unexpected(written.error());
    co_return rotation_report{std::move(key_id), previous, next, encryption_limits()};
}

runtime::task<void> keyring::revoke(std::string key_id, std::uint64_t version) {
    if (!valid_key_id(key_id) || version == 0) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id or version", std::move(key_id)));
    }
    if (!impl_->providers.contains(key_id)) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    if (file->revoked.contains(version))
        co_return result<void>{};
    if (file->versions.erase(version) == 0) {
        const auto reference = identity_detail::versioned_key_id(key_id, version);
        co_return std::unexpected(
            key_error(errc::not_found, "data key version does not exist", reference));
    }
    file->revoked.insert(version);
    auto written = co_await write_atomic(path, encode_wrapped_key_file(*file));
    if (!written)
        co_return std::unexpected(written.error());
    co_return result<void>{};
}

} // namespace liquid
