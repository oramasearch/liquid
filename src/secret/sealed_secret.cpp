// SPDX-License-Identifier: MIT

#include "liquid/secret.hpp"

#include <algorithm>
#include <array>
#include <exception>
#include <ranges>
#include <string_view>
#include <utility>

#include "crypto/crypto.hpp"
#include "encoding/cbor.hpp"
#include "formats/json.hpp"
#include "net/http_client.hpp"
#include "secret/secret.hpp"

namespace liquid {
namespace {

namespace cbor = encoding::cbor;
namespace json = formats::json;

constexpr auto envelope_version = std::uint64_t{1};
constexpr auto authentication_tag_size = std::size_t{16};
constexpr auto kdf_label = std::string_view{"liquid sealed secret v1"};

auto secret_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

class wipe_key {
public:
    explicit wipe_key(crypto_detail::key& value) : value_(&value) {}

    ~wipe_key() {
        crypto_detail::wipe(*value_);
    }

    wipe_key(const wipe_key&) = delete;
    auto operator=(const wipe_key&) -> wipe_key& = delete;

private:
    crypto_detail::key* value_;
};

template <std::size_t Size>
auto byte_vector(const std::array<std::byte, Size>& input) -> std::vector<std::byte> {
    return {input.begin(), input.end()};
}

auto scope_value(const seal_scope& scope) -> cbor::value {
    auto fields = cbor::value::map{{"cluster", scope.cluster}};
    if (scope.namespace_) {
        fields.emplace_back("namespace", *scope.namespace_);
    }
    if (scope.name) {
        fields.emplace_back("name", *scope.name);
    }
    return cbor::value{std::move(fields)};
}

auto record_value(const sealed_secret& value, bool include_ciphertext) -> cbor::value {
    auto fields = cbor::value::map{
        {"v", value.v},
        {"kid", value.kid},
        {"epk", byte_vector(value.epk)},
        {"nonce", byte_vector(value.nonce)},
        {"aad", scope_value(value.scope)},
    };
    if (include_ciphertext) {
        fields.emplace_back("ct", value.ciphertext);
    }
    return cbor::value{std::move(fields)};
}

auto validate_record(const sealed_secret& value, errc code) -> result<void> {
    if (value.v != envelope_version) {
        return std::unexpected(secret_error(code, "sealed-secret version must be 1", "v"));
    }
    if (value.kid.empty()) {
        return std::unexpected(secret_error(code, "sealed-secret key id is empty", "kid"));
    }
    if (value.scope.cluster.empty()) {
        return std::unexpected(secret_error(code, "sealed-secret cluster is empty", "cluster"));
    }
    if (value.ciphertext.size() < authentication_tag_size) {
        return std::unexpected(secret_error(code, "sealed-secret ciphertext is truncated", "ct"));
    }
    return {};
}

auto authenticated_header(const sealed_secret& value) -> result<std::vector<std::byte>> {
    return cbor::encode(record_value(value, false));
}

auto cbor_field(const cbor::value::map& fields, std::string_view name) -> const cbor::value* {
    for (const auto& [key, value] : fields) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto required_map(const cbor::value& value, std::string_view field)
    -> result<const cbor::value::map*> {
    const auto* output = std::get_if<cbor::value::map>(&value.data);
    if (!output) {
        return std::unexpected(
            secret_error(errc::corrupt, "sealed-secret field must be a map", std::string{field}));
    }
    return output;
}

template <class T>
auto required_value(const cbor::value::map& fields, std::string_view name) -> result<const T*> {
    const auto* field = cbor_field(fields, name);
    const auto* output = field ? std::get_if<T>(&field->data) : nullptr;
    if (!output) {
        return std::unexpected(secret_error(errc::corrupt,
                                            "sealed-secret field is missing or has the wrong type",
                                            std::string{name}));
    }
    return output;
}

template <std::size_t Size>
auto fixed_bytes(const cbor::value::map& fields, std::string_view name)
    -> result<std::array<std::byte, Size>> {
    auto value = required_value<cbor::value::bytes>(fields, name);
    if (!value) {
        return std::unexpected(value.error());
    }
    if ((*value)->size() != Size) {
        return std::unexpected(secret_error(
            errc::corrupt, "sealed-secret field has the wrong byte length", std::string{name}));
    }
    auto output = std::array<std::byte, Size>{};
    std::ranges::copy(**value, output.begin());
    return output;
}

auto optional_text(const cbor::value::map& fields, std::string_view name)
    -> result<std::optional<std::string>> {
    const auto* field = cbor_field(fields, name);
    if (!field) {
        return std::optional<std::string>{};
    }
    const auto* text = std::get_if<std::string>(&field->data);
    if (!text) {
        return std::unexpected(secret_error(
            errc::corrupt, "sealed-secret scope field must be text", std::string{name}));
    }
    return std::optional<std::string>{*text};
}

auto validate_scope_fields(const cbor::value::map& fields) -> result<void> {
    if (fields.empty() || fields.size() > 3) {
        return std::unexpected(
            secret_error(errc::corrupt, "sealed-secret aad has unexpected fields", "aad"));
    }
    for (const auto& [key, ignored] : fields) {
        static_cast<void>(ignored);
        const auto* text = std::get_if<std::string>(&key.data);
        if (!text || (*text != "cluster" && *text != "namespace" && *text != "name")) {
            return std::unexpected(
                secret_error(errc::corrupt, "sealed-secret aad has an unknown field", "aad"));
        }
    }
    return {};
}

auto validate_record_fields(const cbor::value::map& fields) -> result<void> {
    if (fields.size() != 6) {
        return std::unexpected(
            secret_error(errc::corrupt, "sealed-secret record has unexpected fields", "record"));
    }
    for (const auto& [key, ignored] : fields) {
        static_cast<void>(ignored);
        const auto* text = std::get_if<std::string>(&key.data);
        if (!text || (*text != "v" && *text != "kid" && *text != "epk" && *text != "nonce" &&
                      *text != "aad" && *text != "ct")) {
            return std::unexpected(
                secret_error(errc::corrupt, "sealed-secret record has an unknown field", "record"));
        }
    }
    return {};
}

auto scope_mismatch(const seal_scope& actual, const seal_scope& expected) -> result<void> {
    if (actual.cluster != expected.cluster) {
        return std::unexpected(
            secret_error(errc::unauthorized, "sealed-secret scope does not match", "cluster"));
    }
    if (actual.namespace_ != expected.namespace_) {
        return std::unexpected(
            secret_error(errc::unauthorized, "sealed-secret scope does not match", "namespace"));
    }
    if (actual.name != expected.name) {
        return std::unexpected(
            secret_error(errc::unauthorized, "sealed-secret scope does not match", "name"));
    }
    return {};
}

auto kdf_info(std::string_view kid) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    output.reserve(kdf_label.size() + 1 + kid.size());
    const auto* label = reinterpret_cast<const std::byte*>(kdf_label.data());
    output.insert(output.end(), label, label + kdf_label.size());
    output.push_back(std::byte{});
    const auto* id = reinterpret_cast<const std::byte*>(kid.data());
    output.insert(output.end(), id, id + kid.size());
    return output;
}

auto kdf_salt(const std::array<std::byte, 32>& ephemeral,
              const std::array<std::byte, 32>& recipient) -> std::array<std::byte, 64> {
    auto output = std::array<std::byte, 64>{};
    std::ranges::copy(ephemeral, output.begin());
    std::ranges::copy(recipient, output.begin() + 32);
    return output;
}

auto json_field(const json::value::object& fields, std::string_view name) -> const json::value* {
    const auto found = std::ranges::find(fields, name, &std::pair<std::string, json::value>::first);
    return found == fields.end() ? nullptr : &found->second;
}

auto json_object(const json::value* value) -> const json::value::object* {
    return value ? std::get_if<json::value::object>(&value->data) : nullptr;
}

auto json_text(const json::value::object& fields, std::string_view name) -> const std::string* {
    const auto* value = json_field(fields, name);
    return value ? std::get_if<std::string>(&value->data) : nullptr;
}

auto lake_error_code(std::string_view code, unsigned status) -> errc {
    if (code == "invalid_input" || status == 400) {
        return errc::invalid_argument;
    }
    if (code == "unauthorized" || code == "mfa_required" || status == 401 || status == 403) {
        return errc::unauthorized;
    }
    if (code == "not_found" || code == "unknown_capability" || status == 404) {
        return errc::not_found;
    }
    if (code == "conflict" || code == "unrecoverable" || status == 409) {
        return errc::conflict;
    }
    if (code == "store_unavailable" || status == 503) {
        return errc::unavailable;
    }
    return errc::internal;
}

auto decode_hex_key(std::string_view text) -> result<std::array<std::byte, 32>> {
    if (text.size() != 64) {
        return std::unexpected(secret_error(
            errc::corrupt, "sealing public key must contain 64 hex characters", "public_key"));
    }
    auto output = std::array<std::byte, 32>{};
    const auto nibble = [](char character) -> int {
        if (character >= '0' && character <= '9') {
            return character - '0';
        }
        if (character >= 'a' && character <= 'f') {
            return character - 'a' + 10;
        }
        if (character >= 'A' && character <= 'F') {
            return character - 'A' + 10;
        }
        return -1;
    };
    for (auto index = std::size_t{}; index < output.size(); ++index) {
        const auto high = nibble(text[index * 2]);
        const auto low = nibble(text[index * 2 + 1]);
        if (high < 0 || low < 0) {
            return std::unexpected(secret_error(
                errc::corrupt, "sealing public key contains invalid hex", "public_key"));
        }
        output[index] = std::byte{static_cast<unsigned char>((high << 4) | low)};
    }
    return output;
}

auto decode_lake_response(const net_detail::http_response& response) -> result<sealing_public_key> {
    const auto body =
        std::string{reinterpret_cast<const char*>(response.body.data()), response.body.size()};
    auto parsed = json::parse(body);
    const auto* envelope = parsed ? std::get_if<json::value::object>(&parsed->data) : nullptr;
    const auto* ok_value = envelope ? json_field(*envelope, "ok") : nullptr;
    const auto* ok = ok_value ? std::get_if<bool>(&ok_value->data) : nullptr;
    if (!envelope || !ok) {
        return std::unexpected(secret_error(response.status >= 200 && response.status < 300
                                                ? errc::corrupt
                                                : lake_error_code({}, response.status),
                                            "lake returned an invalid capability envelope",
                                            "sealing_public_key"));
    }
    if (!*ok) {
        const auto* fields = json_object(json_field(*envelope, "error"));
        const auto* code = fields ? json_text(*fields, "code") : nullptr;
        const auto* message = fields ? json_text(*fields, "message") : nullptr;
        const auto* path = fields ? json_text(*fields, "path") : nullptr;
        if (!code || !message) {
            return std::unexpected(secret_error(lake_error_code({}, response.status),
                                                "lake returned an invalid error envelope",
                                                "sealing_public_key"));
        }
        return std::unexpected(secret_error(lake_error_code(*code, response.status),
                                            *message,
                                            path ? *path : "sealing_public_key"));
    }
    if (response.status < 200 || response.status >= 300) {
        return std::unexpected(secret_error(lake_error_code({}, response.status),
                                            "lake returned an unsuccessful HTTP status",
                                            "sealing_public_key"));
    }
    const auto* value = json_object(json_field(*envelope, "value"));
    const auto* kid = value ? json_text(*value, "kid") : nullptr;
    const auto* key = value ? json_text(*value, "public_key") : nullptr;
    const auto* algorithm = value ? json_text(*value, "algorithm") : nullptr;
    if (!kid || kid->empty() || !key || !algorithm || *algorithm != "x25519") {
        return std::unexpected(secret_error(
            errc::corrupt, "lake returned an invalid sealing public key", "sealing_public_key"));
    }
    auto decoded = decode_hex_key(*key);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    return sealing_public_key{.kid = *kid, .key = *decoded};
}

} // namespace

auto encode_sealed_secret(const sealed_secret& value) -> result<std::vector<std::byte>> {
    try {
        auto valid = validate_record(value, errc::invalid_argument);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        return cbor::encode(record_value(value, true));
    } catch (const std::exception& exception) {
        return std::unexpected(
            secret_error(errc::internal, exception.what(), "sealed secret encode"));
    }
}

auto decode_sealed_secret(std::span<const std::byte> input) -> result<sealed_secret> {
    try {
        auto decoded = cbor::decode(input);
        if (!decoded) {
            return std::unexpected(decoded.error());
        }
        auto canonical = cbor::encode(*decoded);
        if (!canonical || !std::ranges::equal(*canonical, input)) {
            return std::unexpected(secret_error(
                errc::corrupt, "sealed-secret record is not canonical CBOR", "record"));
        }
        auto fields = required_map(*decoded, "record");
        if (!fields) {
            return std::unexpected(fields.error());
        }
        auto valid_fields = validate_record_fields(**fields);
        if (!valid_fields) {
            return std::unexpected(valid_fields.error());
        }
        auto version = required_value<std::uint64_t>(**fields, "v");
        auto kid = required_value<std::string>(**fields, "kid");
        auto epk = fixed_bytes<32>(**fields, "epk");
        auto nonce = fixed_bytes<24>(**fields, "nonce");
        auto ciphertext = required_value<cbor::value::bytes>(**fields, "ct");
        const auto* aad_value = cbor_field(**fields, "aad");
        if (!version || !kid || !epk || !nonce || !ciphertext || !aad_value) {
            const auto failure =
                !version      ? version.error()
                : !kid        ? kid.error()
                : !epk        ? epk.error()
                : !nonce      ? nonce.error()
                : !ciphertext ? ciphertext.error()
                              : secret_error(errc::corrupt, "sealed-secret aad is missing", "aad");
            return std::unexpected(failure);
        }
        auto aad = required_map(*aad_value, "aad");
        if (!aad) {
            return std::unexpected(aad.error());
        }
        auto valid_aad = validate_scope_fields(**aad);
        if (!valid_aad) {
            return std::unexpected(valid_aad.error());
        }
        auto cluster = required_value<std::string>(**aad, "cluster");
        auto namespace_ = optional_text(**aad, "namespace");
        auto name = optional_text(**aad, "name");
        if (!cluster || !namespace_ || !name) {
            const auto failure = !cluster      ? cluster.error()
                                 : !namespace_ ? namespace_.error()
                                               : name.error();
            return std::unexpected(failure);
        }
        auto output = sealed_secret{
            .v = **version,
            .kid = **kid,
            .epk = *epk,
            .nonce = *nonce,
            .scope = seal_scope{.cluster = **cluster,
                                .namespace_ = std::move(*namespace_),
                                .name = std::move(*name)},
            .ciphertext = **ciphertext,
        };
        auto valid = validate_record(output, errc::corrupt);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(
            secret_error(errc::internal, exception.what(), "sealed secret decode"));
    }
}

auto secret_detail::seal_secret_with_random(const sealing_public_key& recipient,
                                            seal_scope scope,
                                            std::span<const std::byte> plaintext,
                                            const random_source& random) -> result<sealed_secret> {
    try {
        auto output =
            sealed_secret{.v = envelope_version, .kid = recipient.kid, .scope = std::move(scope)};
        if (output.kid.empty()) {
            return std::unexpected(
                secret_error(errc::invalid_argument, "sealing key id is empty", "kid"));
        }
        if (output.scope.cluster.empty()) {
            return std::unexpected(
                secret_error(errc::invalid_argument, "sealed-secret cluster is empty", "cluster"));
        }
        auto ephemeral_private = crypto_detail::key{};
        auto ephemeral_guard = wipe_key{ephemeral_private};
        auto generated = random(ephemeral_private);
        if (!generated) {
            return std::unexpected(generated.error());
        }
        auto ephemeral_public = crypto_detail::x25519_public_key(ephemeral_private);
        if (!ephemeral_public) {
            return std::unexpected(ephemeral_public.error());
        }
        output.epk = *ephemeral_public;
        auto shared_result = crypto_detail::x25519(ephemeral_private, recipient.key);
        if (!shared_result) {
            return std::unexpected(shared_result.error());
        }
        auto shared = *shared_result;
        auto shared_guard = wipe_key{shared};
        if (crypto_detail::all_zero(shared)) {
            return std::unexpected(secret_error(
                errc::unauthorized, "X25519 produced an all-zero shared secret", "public_key"));
        }
        auto salt = kdf_salt(output.epk, recipient.key);
        auto info = kdf_info(output.kid);
        auto key_result = crypto_detail::hkdf_sha512(salt, shared, info);
        if (!key_result) {
            return std::unexpected(key_result.error());
        }
        auto key = *key_result;
        auto key_guard = wipe_key{key};
        generated = random(output.nonce);
        if (!generated) {
            return std::unexpected(generated.error());
        }
        auto ad = authenticated_header(output);
        if (!ad) {
            return std::unexpected(ad.error());
        }
        auto ciphertext = crypto_detail::xchacha20_poly1305_seal(key, output.nonce, *ad, plaintext);
        if (!ciphertext) {
            return std::unexpected(ciphertext.error());
        }
        output.ciphertext = std::move(*ciphertext);
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(secret_error(errc::internal, exception.what(), "seal secret"));
    }
}

auto seal_secret(const sealing_public_key& recipient,
                 seal_scope scope,
                 std::span<const std::byte> plaintext) -> result<sealed_secret> {
    return secret_detail::seal_secret_with_random(
        recipient, std::move(scope), plaintext, [](std::span<std::byte> output) {
            return crypto_detail::random_bytes(output);
        });
}

auto unseal_secret(std::span<const std::byte, 32> private_key,
                   const sealed_secret& value,
                   const seal_scope& expected) -> result<std::vector<std::byte>> {
    try {
        auto valid = validate_record(value, errc::corrupt);
        if (!valid) {
            return std::unexpected(valid.error());
        }
        auto scope = scope_mismatch(value.scope, expected);
        if (!scope) {
            return std::unexpected(scope.error());
        }
        auto recipient_private = crypto_detail::key{};
        std::ranges::copy(private_key, recipient_private.begin());
        auto private_guard = wipe_key{recipient_private};
        auto recipient_public = crypto_detail::x25519_public_key(recipient_private);
        if (!recipient_public) {
            return std::unexpected(recipient_public.error());
        }
        auto shared_result = crypto_detail::x25519(recipient_private, value.epk);
        if (!shared_result) {
            return std::unexpected(shared_result.error());
        }
        auto shared = *shared_result;
        auto shared_guard = wipe_key{shared};
        if (crypto_detail::all_zero(shared)) {
            return std::unexpected(secret_error(
                errc::unauthorized, "X25519 produced an all-zero shared secret", "epk"));
        }
        auto salt = kdf_salt(value.epk, *recipient_public);
        auto info = kdf_info(value.kid);
        auto key_result = crypto_detail::hkdf_sha512(salt, shared, info);
        if (!key_result) {
            return std::unexpected(key_result.error());
        }
        auto key = *key_result;
        auto key_guard = wipe_key{key};
        auto ad = authenticated_header(value);
        if (!ad) {
            return std::unexpected(ad.error());
        }
        auto plaintext =
            crypto_detail::xchacha20_poly1305_open(key, value.nonce, *ad, value.ciphertext);
        if (!plaintext) {
            return std::unexpected(secret_error(
                errc::unauthorized, "sealed-secret authentication failed", "ciphertext"));
        }
        return plaintext;
    } catch (const std::exception& exception) {
        return std::unexpected(secret_error(errc::internal, exception.what(), "unseal secret"));
    }
}

auto generate_sealing_keypair() -> result<sealing_keypair> {
    try {
        auto private_key = crypto_detail::random_key();
        if (!private_key) {
            return std::unexpected(private_key.error());
        }
        auto public_key = crypto_detail::x25519_public_key(*private_key);
        if (!public_key) {
            crypto_detail::wipe(*private_key);
            return std::unexpected(public_key.error());
        }
        auto output = sealing_keypair{
            .public_key = sealing_public_key{.kid = {}, .key = *public_key},
            .private_key = *private_key,
        };
        crypto_detail::wipe(*private_key);
        return output;
    } catch (const std::exception& exception) {
        return std::unexpected(
            secret_error(errc::internal, exception.what(), "generate sealing keypair"));
    }
}

auto secret_detail::fetch_sealing_public_key_with(std::string lake_url,
                                                  std::string bearer,
                                                  const http_requester& request)
    -> runtime::task<sealing_public_key> {
    try {
        auto parsed = net_detail::parse_url(lake_url, "lake URL");
        if (!parsed) {
            co_return std::unexpected(parsed.error());
        }
        auto response = co_await request(net_detail::http_request{
            .method = net_detail::http_method::get,
            .url = *parsed,
            .target = net_detail::request_target(*parsed, "/v1/sealing_public_key"),
            .bearer = std::move(bearer),
            .content_type = "application/json",
        });
        if (!response) {
            co_return std::unexpected(response.error());
        }
        co_return decode_lake_response(*response);
    } catch (const std::exception& exception) {
        co_return std::unexpected(
            secret_error(errc::internal, exception.what(), "fetch sealing public key"));
    }
}

auto fetch_sealing_public_key(std::string lake_url, std::string bearer)
    -> runtime::task<sealing_public_key> {
    co_return co_await secret_detail::fetch_sealing_public_key_with(
        std::move(lake_url), std::move(bearer), [](net_detail::http_request request) {
            return net_detail::perform_request(std::move(request));
        });
}

} // namespace liquid
