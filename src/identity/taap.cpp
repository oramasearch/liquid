#include "identity/taap.hpp"

#include <array>
#include <chrono>
#include <format>
#include <limits>

#include "encoding/cbor.hpp"
#include "monocypher-ed25519.h"

namespace liquid::identity_detail {
namespace {

namespace cbor = encoding::cbor;

constexpr std::array<std::uint64_t, 8> sha512_256_initial_values{
    0x22312194FC2BF72CULL,
    0x9F555FA3C84C64C2ULL,
    0x2393B86B6F53B151ULL,
    0x963877195940EABDULL,
    0x96283EE2A88EFFE3ULL,
    0xBE5E1E2553863992ULL,
    0x2B0199FC2C85B8AAULL,
    0x0EB72DDC81C52CA2ULL,
};

auto failure(errc code, std::string message) -> error {
    return {code, std::move(message), "TAAP/1"};
}

auto byte_string(std::span<const std::byte> input) -> cbor::value {
    return cbor::value{cbor::value::bytes{input.begin(), input.end()}};
}

auto map(std::initializer_list<std::pair<cbor::value, cbor::value>> fields) -> cbor::value {
    return cbor::value{cbor::value::map{fields}};
}

auto field(const cbor::value& source, std::string_view name) -> const cbor::value* {
    const auto* values = std::get_if<cbor::value::map>(&source.data);
    if (!values) {
        return nullptr;
    }
    for (const auto& [key, value] : *values) {
        const auto* text = std::get_if<std::string>(&key.data);
        if (text && *text == name) {
            return &value;
        }
    }
    return nullptr;
}

auto public_key_object(std::span<const std::byte> public_key,
                       std::string_view created_at,
                       std::string_view key_label) -> cbor::value {
    auto usage = cbor::value::array{};
    usage.emplace_back("signing");
    return map({{"key_algorithm", "ed25519"},
                {"key_usage", cbor::value{std::move(usage)}},
                {"public_key_bytes_base64", base64(public_key)},
                {"created_at", std::string{created_at}},
                {"key_label", std::string{key_label}}});
}

auto parse_actor_identifier(std::string_view value, actor& output) -> result<void> {
    constexpr std::string_view prefix = "taap:1:";
    if (!value.starts_with(prefix)) {
        return std::unexpected(failure(errc::invalid_argument, "invalid TAAP identifier"));
    }
    const auto algorithm_end = value.find(':', prefix.size());
    if (algorithm_end == std::string_view::npos || algorithm_end == prefix.size() ||
        algorithm_end + 1 == value.size()) {
        return std::unexpected(failure(errc::invalid_argument, "invalid TAAP identifier"));
    }
    output.taap_id = value;
    output.algorithm = value.substr(prefix.size(), algorithm_end - prefix.size());
    output.fingerprint = value.substr(algorithm_end + 1);
    return {};
}

auto signature_value(const signature& value) -> cbor::value {
    return map({{"signature_algorithm", value.algorithm},
                {"signing_key_identifier", value.signing_key_identifier},
                {"signing_key_label", value.signing_key_label},
                {"signature_purpose", value.signature_purpose},
                {"signed_at", value.signed_at},
                {"signature_bytes_base64", base64(value.bytes)}});
}

auto compromise_value(const compromise_declaration& value) -> cbor::value {
    auto fields = cbor::value::map{{"key_fingerprint_base32", value.key_fingerprint_base32},
                                   {"declared_at", value.declared_at}};
    if (value.reason) {
        fields.emplace_back("reason", *value.reason);
    }
    return cbor::value{std::move(fields)};
}

auto decode_base64(std::string_view input) -> result<std::vector<std::byte>> {
    if (input.empty() || input.size() % 4 != 0) {
        return std::unexpected(failure(errc::corrupt, "invalid base64"));
    }
    auto value = [](char character) -> int {
        if (character >= 'A' && character <= 'Z') {
            return character - 'A';
        }
        if (character >= 'a' && character <= 'z') {
            return character - 'a' + 26;
        }
        if (character >= '0' && character <= '9') {
            return character - '0' + 52;
        }
        if (character == '+') {
            return 62;
        }
        if (character == '/') {
            return 63;
        }
        return -1;
    };
    auto output = std::vector<std::byte>{};
    for (std::size_t index = 0; index < input.size(); index += 4) {
        const auto first = value(input[index]);
        const auto second = value(input[index + 1]);
        const auto third = input[index + 2] == '=' ? -2 : value(input[index + 2]);
        const auto fourth = input[index + 3] == '=' ? -2 : value(input[index + 3]);
        if (first < 0 || second < 0 || third == -1 || fourth == -1 ||
            (third == -2 && fourth != -2) ||
            ((third == -2 || fourth == -2) && index + 4 != input.size())) {
            return std::unexpected(failure(errc::corrupt, "invalid base64"));
        }
        const auto group = (static_cast<std::uint32_t>(first) << 18U) |
                           (static_cast<std::uint32_t>(second) << 12U) |
                           (third < 0 ? 0U : static_cast<std::uint32_t>(third) << 6U) |
                           (fourth < 0 ? 0U : static_cast<std::uint32_t>(fourth));
        output.push_back(std::byte{static_cast<unsigned char>(group >> 16U)});
        if (third >= 0) {
            output.push_back(std::byte{static_cast<unsigned char>(group >> 8U)});
        }
        if (fourth >= 0) {
            output.push_back(std::byte{static_cast<unsigned char>(group)});
        }
    }
    return output;
}

} // namespace

auto sha512_256(std::span<const std::byte> input) -> std::array<std::byte, 32> {
    auto context = crypto_sha512_ctx{};
    crypto_sha512_init(&context);
    for (std::size_t index = 0; index != sha512_256_initial_values.size(); ++index) {
        context.hash[index] = sha512_256_initial_values[index];
    }
    crypto_sha512_update(
        &context, reinterpret_cast<const std::uint8_t*>(input.data()), input.size());
    auto full = std::array<std::byte, 64>{};
    crypto_sha512_final(&context, reinterpret_cast<std::uint8_t*>(full.data()));
    auto output = std::array<std::byte, 32>{};
    std::ranges::copy_n(full.begin(), output.size(), output.begin());
    return output;
}

auto base32(std::span<const std::byte> input) -> std::string {
    constexpr std::string_view alphabet = "ABCDEFGHIJKLMNOPQRSTUVWXYZ234567";
    auto output = std::string{};
    auto buffer = std::uint32_t{};
    auto bits = 0U;
    for (const auto byte : input) {
        buffer = (buffer << 8U) | std::to_integer<std::uint8_t>(byte);
        bits += 8;
        while (bits >= 5) {
            bits -= 5;
            output.push_back(alphabet[(buffer >> bits) & 0x1fU]);
        }
    }
    if (bits != 0) {
        output.push_back(alphabet[(buffer << (5U - bits)) & 0x1fU]);
    }
    return output;
}

auto base64(std::span<const std::byte> input) -> std::string {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    auto output = std::string{};
    for (std::size_t index = 0; index < input.size(); index += 3) {
        const auto remaining = input.size() - index;
        const auto group =
            (std::uint32_t{std::to_integer<std::uint8_t>(input[index])} << 16U) |
            (remaining > 1 ? std::uint32_t{std::to_integer<std::uint8_t>(input[index + 1])} << 8U
                           : 0U) |
            (remaining > 2 ? std::to_integer<std::uint8_t>(input[index + 2]) : 0U);
        output.push_back(alphabet[(group >> 18U) & 0x3fU]);
        output.push_back(alphabet[(group >> 12U) & 0x3fU]);
        output.push_back(remaining > 1 ? alphabet[(group >> 6U) & 0x3fU] : '=');
        output.push_back(remaining > 2 ? alphabet[group & 0x3fU] : '=');
    }
    return output;
}

auto identifier(std::span<const std::byte> public_key,
                std::string_view created_at,
                std::string_view key_label) -> result<actor> {
    auto fingerprint = key_fingerprint(public_key, created_at, key_label);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    auto output = actor{};
    output.algorithm = "ed25519";
    output.fingerprint = *fingerprint;
    output.key_fingerprint = *fingerprint;
    output.taap_id = "taap:1:ed25519:" + output.fingerprint;
    output.public_key.assign(public_key.begin(), public_key.end());
    return output;
}

auto key_fingerprint(std::span<const std::byte> public_key,
                     std::string_view created_at,
                     std::string_view key_label) -> result<std::string> {
    if (public_key.size() != 32) {
        return std::unexpected(
            failure(errc::invalid_argument, "Ed25519 public key must be 32 bytes"));
    }
    auto encoded = cbor::encode(public_key_object(public_key, created_at, key_label));
    if (!encoded) {
        return std::unexpected(encoded.error());
    }
    return base32(sha512_256(*encoded));
}

auto canonical_signature(const signature& value) -> result<std::vector<std::byte>> {
    if (value.algorithm != "ed25519" || value.bytes.size() != 64) {
        return std::unexpected(failure(errc::invalid_argument, "invalid TAAP signature object"));
    }
    return cbor::encode(signature_value(value));
}

auto parse_signature(std::span<const std::byte> input) -> result<signature> {
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* algorithm = field(*decoded, "signature_algorithm");
    const auto* identifier = field(*decoded, "signing_key_identifier");
    const auto* label = field(*decoded, "signing_key_label");
    const auto* purpose = field(*decoded, "signature_purpose");
    const auto* signed_at = field(*decoded, "signed_at");
    const auto* bytes = field(*decoded, "signature_bytes_base64");
    const auto* algorithm_text = algorithm ? std::get_if<std::string>(&algorithm->data) : nullptr;
    const auto* identifier_text =
        identifier ? std::get_if<std::string>(&identifier->data) : nullptr;
    const auto* label_text = label ? std::get_if<std::string>(&label->data) : nullptr;
    const auto* purpose_text = purpose ? std::get_if<std::string>(&purpose->data) : nullptr;
    const auto* time_text = signed_at ? std::get_if<std::string>(&signed_at->data) : nullptr;
    const auto* encoded = bytes ? std::get_if<std::string>(&bytes->data) : nullptr;
    if (!algorithm_text || !identifier_text || !label_text || !purpose_text || !time_text ||
        !encoded) {
        return std::unexpected(failure(errc::corrupt, "invalid TAAP signature object"));
    }
    auto raw = decode_base64(*encoded);
    if (!raw || raw->size() != 64) {
        return std::unexpected(failure(errc::corrupt, "invalid TAAP signature bytes"));
    }
    return signature{
        *algorithm_text, *identifier_text, *label_text, *purpose_text, *time_text, std::move(*raw)};
}

auto canonical_document(const identity_document& document, bool include_signatures)
    -> result<std::vector<std::byte>> {
    auto keys = cbor::value::array{};
    keys.push_back(
        public_key_object(document.subject.public_key, document.created_at, document.key_label));
    auto fields = cbor::value::map{{"artifact_type", "identity_document"},
                                   {"protocol_version", "TAAP/1"},
                                   {"encoding", "ccbor"},
                                   {"identifier", document.subject.taap_id},
                                   {"keys", cbor::value{std::move(keys)}},
                                   {"controller", document.subject.taap_id},
                                   {"previous_document_hash_base64",
                                    document.previous_document_hash
                                        ? cbor::value{base64(*document.previous_document_hash)}
                                        : cbor::value{cbor::null{}}},
                                   {"created_at", document.created_at},
                                   {"updated_at", document.updated_at}};
    if (!document.compromised_keys.empty()) {
        auto compromised = cbor::value::array{};
        for (const auto& declaration : document.compromised_keys) {
            compromised.push_back(compromise_value(declaration));
        }
        fields.emplace_back("compromised_keys", cbor::value{std::move(compromised)});
    }
    if (include_signatures) {
        auto signatures = cbor::value::array{};
        for (const auto& item : document.signatures) {
            signatures.push_back(signature_value(item));
        }
        fields.emplace_back("signatures", cbor::value{std::move(signatures)});
    }
    return cbor::encode(cbor::value{std::move(fields)});
}

auto parse_document(std::span<const std::byte> input) -> result<identity_document> {
    auto decoded = cbor::decode(input);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* artifact_type = field(*decoded, "artifact_type");
    const auto* protocol_version = field(*decoded, "protocol_version");
    const auto* encoding = field(*decoded, "encoding");
    const auto* identifier_value = field(*decoded, "identifier");
    const auto* keys_value = field(*decoded, "keys");
    const auto* created_at = field(*decoded, "created_at");
    const auto* updated_at = field(*decoded, "updated_at");
    const auto* previous = field(*decoded, "previous_document_hash_base64");
    const auto* signatures_value = field(*decoded, "signatures");
    const auto* artifact_text =
        artifact_type ? std::get_if<std::string>(&artifact_type->data) : nullptr;
    const auto* protocol_text =
        protocol_version ? std::get_if<std::string>(&protocol_version->data) : nullptr;
    const auto* encoding_text = encoding ? std::get_if<std::string>(&encoding->data) : nullptr;
    const auto* identifier_text =
        identifier_value ? std::get_if<std::string>(&identifier_value->data) : nullptr;
    const auto* keys = keys_value ? std::get_if<cbor::value::array>(&keys_value->data) : nullptr;
    const auto* created_text = created_at ? std::get_if<std::string>(&created_at->data) : nullptr;
    const auto* updated_text = updated_at ? std::get_if<std::string>(&updated_at->data) : nullptr;
    const auto* signatures =
        signatures_value ? std::get_if<cbor::value::array>(&signatures_value->data) : nullptr;
    if (!artifact_text || *artifact_text != "identity_document" || !protocol_text ||
        *protocol_text != "TAAP/1" || !encoding_text || *encoding_text != "ccbor" ||
        !identifier_text || !keys || keys->size() != 1 || !created_text || !updated_text ||
        !previous || !signatures) {
        return std::unexpected(failure(errc::corrupt, "invalid identity document"));
    }
    const auto* key = std::get_if<cbor::value::map>(&keys->front().data);
    if (!key) {
        return std::unexpected(failure(errc::corrupt, "invalid identity document key"));
    }
    const auto key_field = [&](std::string_view name) -> const cbor::value* {
        for (const auto& [candidate, value] : *key) {
            const auto* text = std::get_if<std::string>(&candidate.data);
            if (text && *text == name) {
                return &value;
            }
        }
        return nullptr;
    };
    const auto* key_algorithm = key_field("key_algorithm");
    const auto* key_label = key_field("key_label");
    const auto* encoded_public_key = key_field("public_key_bytes_base64");
    const auto* key_algorithm_text =
        key_algorithm ? std::get_if<std::string>(&key_algorithm->data) : nullptr;
    const auto* key_label_text = key_label ? std::get_if<std::string>(&key_label->data) : nullptr;
    const auto* encoded_public_key_text =
        encoded_public_key ? std::get_if<std::string>(&encoded_public_key->data) : nullptr;
    if (!key_algorithm_text || *key_algorithm_text != "ed25519" || !key_label_text ||
        !encoded_public_key_text) {
        return std::unexpected(failure(errc::corrupt, "invalid identity document key"));
    }
    auto public_key = decode_base64(*encoded_public_key_text);
    if (!public_key || public_key->size() != 32) {
        return std::unexpected(failure(errc::corrupt, "invalid identity document public key"));
    }
    auto subject = actor{};
    auto parsed_identifier = parse_actor_identifier(*identifier_text, subject);
    if (!parsed_identifier) {
        return std::unexpected(parsed_identifier.error());
    }
    subject.public_key = std::move(*public_key);
    auto fingerprint = key_fingerprint(subject.public_key, *created_text, *key_label_text);
    if (!fingerprint) {
        return std::unexpected(fingerprint.error());
    }
    subject.key_fingerprint = std::move(*fingerprint);
    auto output = identity_document{
        subject, *key_label_text, *created_text, *updated_text, std::nullopt, {}, {}};
    if (std::holds_alternative<cbor::null>(previous->data)) {
        output.previous_document_hash = std::nullopt;
    } else {
        const auto* previous_text = std::get_if<std::string>(&previous->data);
        if (!previous_text) {
            return std::unexpected(failure(errc::corrupt, "invalid predecessor hash"));
        }
        auto previous_hash = decode_base64(*previous_text);
        if (!previous_hash || previous_hash->size() != 32) {
            return std::unexpected(failure(errc::corrupt, "invalid predecessor hash"));
        }
        output.previous_document_hash = std::move(*previous_hash);
    }
    if (const auto* compromised_value = field(*decoded, "compromised_keys")) {
        const auto* compromised = std::get_if<cbor::value::array>(&compromised_value->data);
        if (!compromised) {
            return std::unexpected(failure(errc::corrupt, "invalid compromised keys"));
        }
        for (const auto& declaration_value : *compromised) {
            const auto* declaration = std::get_if<cbor::value::map>(&declaration_value.data);
            if (!declaration) {
                return std::unexpected(failure(errc::corrupt, "invalid compromised key"));
            }
            const auto declaration_field = [&](std::string_view name) -> const cbor::value* {
                for (const auto& [candidate, value] : *declaration) {
                    const auto* text = std::get_if<std::string>(&candidate.data);
                    if (text && *text == name) {
                        return &value;
                    }
                }
                return nullptr;
            };
            const auto* fingerprint_value = declaration_field("key_fingerprint_base32");
            const auto* declared_at_value = declaration_field("declared_at");
            const auto* reason_value = declaration_field("reason");
            const auto* fingerprint_text =
                fingerprint_value ? std::get_if<std::string>(&fingerprint_value->data) : nullptr;
            const auto* declared_at_text =
                declared_at_value ? std::get_if<std::string>(&declared_at_value->data) : nullptr;
            if (!fingerprint_text || !declared_at_text) {
                return std::unexpected(failure(errc::corrupt, "invalid compromised key"));
            }
            auto reason = std::optional<std::string>{};
            if (reason_value) {
                const auto* reason_text = std::get_if<std::string>(&reason_value->data);
                if (!reason_text) {
                    return std::unexpected(failure(errc::corrupt, "invalid compromised key"));
                }
                reason = *reason_text;
            }
            output.compromised_keys.push_back(
                {*fingerprint_text, *declared_at_text, std::move(reason)});
        }
    }
    for (const auto& signature_value : *signatures) {
        auto encoded_signature = cbor::encode(signature_value);
        if (!encoded_signature) {
            return std::unexpected(encoded_signature.error());
        }
        auto parsed_signature = parse_signature(*encoded_signature);
        if (!parsed_signature) {
            return std::unexpected(parsed_signature.error());
        }
        output.signatures.push_back(std::move(*parsed_signature));
    }
    auto canonical = canonical_document(output, true);
    if (!canonical || !std::ranges::equal(*canonical, input)) {
        return std::unexpected(failure(errc::corrupt, "identity document is not canonical"));
    }
    return output;
}

auto signing_input(std::string_view artifact_type, std::span<const std::byte> canonical_body)
    -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    const auto domain = "TAAP/1 " + std::string{artifact_type};
    output.insert(output.end(),
                  reinterpret_cast<const std::byte*>(domain.data()),
                  reinterpret_cast<const std::byte*>(domain.data() + domain.size()));
    output.push_back(std::byte{0});
    output.insert(output.end(), canonical_body.begin(), canonical_body.end());
    return output;
}

auto now() -> std::string {
    const auto value = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    return std::format("{:%FT%TZ}", value);
}

} // namespace liquid::identity_detail
