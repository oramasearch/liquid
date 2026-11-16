// SPDX-License-Identifier: MIT

#include "liquid/x509.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <cstdint>
#include <optional>
#include <utility>

#include "crypto/x509_internal.hpp"
#include "liquid/asn1.hpp"

namespace liquid::x509 {
namespace {

using asn1::element;
using asn1::reader;
using asn1::tag;
using asn1::tag_class;

constexpr tag sequence{tag_class::universal, 16, true};
constexpr tag set{tag_class::universal, 17, true};
constexpr tag integer_tag{tag_class::universal, 2, false};
constexpr tag boolean_tag{tag_class::universal, 1, false};
constexpr tag bit_string_tag{tag_class::universal, 3, false};
constexpr tag octet_string_tag{tag_class::universal, 4, false};

auto corrupt(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "X.509 certificate"};
}

auto unsupported(std::string message) -> error {
    return error{errc::unsupported, std::move(message), "X.509 certificate"};
}

auto copy(std::span<const std::byte> input) -> std::vector<std::byte> {
    return {input.begin(), input.end()};
}

auto next_required(reader& input, std::string_view name) -> result<element> {
    auto value = input.next();
    if (!value) {
        return std::unexpected(value.error());
    }
    if (!*value) {
        return std::unexpected(corrupt("missing " + std::string(name)));
    }
    return **value;
}

auto expect(const element& value, tag expected, std::string_view name) -> result<void> {
    if (value.identifier != expected) {
        return std::unexpected(corrupt(std::string(name) + " has the wrong DER tag"));
    }
    return {};
}

auto oid(const element& value) -> result<std::string> {
    auto output = asn1::object_identifier(value);
    if (!output) {
        return std::unexpected(corrupt(output.error().message));
    }
    return output;
}

auto parse_decimal(std::string_view text, std::size_t offset, std::size_t count)
    -> std::optional<unsigned int> {
    unsigned int output = 0;
    if (offset + count > text.size()) {
        return std::nullopt;
    }
    for (std::size_t index = offset; index < offset + count; ++index) {
        if (text[index] < '0' || text[index] > '9') {
            return std::nullopt;
        }
        output = output * 10 + static_cast<unsigned int>(text[index] - '0');
    }
    return output;
}

auto parse_time(const element& value) -> result<std::chrono::sys_seconds> {
    const bool utc = value.identifier == tag{tag_class::universal, 23, false};
    const bool generalized = value.identifier == tag{tag_class::universal, 24, false};
    if (!utc && !generalized) {
        return std::unexpected(corrupt("validity time has the wrong DER tag"));
    }
    const std::string_view text(reinterpret_cast<const char*>(value.content.data()),
                                value.content.size());
    if (text.size() != (utc ? 13U : 15U) || text.back() != 'Z') {
        return std::unexpected(corrupt("validity time must contain seconds and UTC Z"));
    }
    const auto encoded_year = parse_decimal(text, 0, utc ? 2 : 4);
    const auto month = parse_decimal(text, utc ? 2 : 4, 2);
    const auto day = parse_decimal(text, utc ? 4 : 6, 2);
    const auto hour = parse_decimal(text, utc ? 6 : 8, 2);
    const auto minute = parse_decimal(text, utc ? 8 : 10, 2);
    const auto second = parse_decimal(text, utc ? 10 : 12, 2);
    if (!encoded_year || !month || !day || !hour || !minute || !second || *hour > 23 ||
        *minute > 59 || *second > 59) {
        return std::unexpected(corrupt("validity time is invalid"));
    }
    const int year_value =
        utc ? (*encoded_year >= 50 ? 1900 : 2000) + *encoded_year : static_cast<int>(*encoded_year);
    const std::chrono::year_month_day date{
        std::chrono::year{year_value}, std::chrono::month{*month}, std::chrono::day{*day}};
    if (!date.ok()) {
        return std::unexpected(corrupt("validity date is invalid"));
    }
    return std::chrono::sys_days{date} + std::chrono::hours{*hour} + std::chrono::minutes{*minute} +
           std::chrono::seconds{*second};
}

auto string_value(const element& value) -> std::string {
    switch (value.identifier.number) {
    case 12:
    case 19:
    case 20:
    case 22:
    case 26:
    case 30:
        return std::string(reinterpret_cast<const char*>(value.content.data()),
                           value.content.size());
    default:
        return "#";
    }
}

auto name(const element& value) -> result<std::string> {
    auto valid = expect(value, sequence, "name");
    if (!valid) {
        return std::unexpected(valid.error());
    }
    reader rdns(value.content);
    std::string output;
    while (!rdns.empty()) {
        auto rdn = next_required(rdns, "relative distinguished name");
        if (!rdn || rdn->identifier != set) {
            return std::unexpected(corrupt("relative distinguished name is invalid"));
        }
        reader attributes(rdn->content);
        while (!attributes.empty()) {
            auto attribute = next_required(attributes, "name attribute");
            if (!attribute || attribute->identifier != sequence) {
                return std::unexpected(corrupt("name attribute is invalid"));
            }
            reader parts(attribute->content);
            auto identifier = next_required(parts, "name attribute identifier");
            auto value_part = next_required(parts, "name attribute value");
            if (!identifier || !value_part || !parts.empty()) {
                return std::unexpected(corrupt("name attribute has invalid fields"));
            }
            auto identifier_text = oid(*identifier);
            if (!identifier_text) {
                return std::unexpected(identifier_text.error());
            }
            if (!output.empty()) {
                output += ",";
            }
            output += *identifier_text + "=" + string_value(*value_part);
        }
    }
    return output;
}

auto algorithm_identifier(const element& value)
    -> result<std::pair<std::string, std::optional<element>>> {
    auto valid = expect(value, sequence, "algorithm identifier");
    if (!valid) {
        return std::unexpected(valid.error());
    }
    reader fields(value.content);
    auto identifier = next_required(fields, "algorithm object identifier");
    if (!identifier) {
        return std::unexpected(identifier.error());
    }
    auto identifier_text = oid(*identifier);
    if (!identifier_text) {
        return std::unexpected(identifier_text.error());
    }
    auto parameters = fields.next();
    if (!parameters) {
        return std::unexpected(parameters.error());
    }
    if (!fields.empty()) {
        return std::unexpected(corrupt("algorithm identifier has trailing fields"));
    }
    return std::pair{*identifier_text, *parameters};
}

auto digest_algorithm(const element& value) -> result<crypto::digest_kind> {
    auto algorithm = algorithm_identifier(value);
    if (!algorithm) {
        return std::unexpected(algorithm.error());
    }
    if (algorithm->second && algorithm->second->identifier != tag{tag_class::universal, 5, false}) {
        return std::unexpected(corrupt("digest algorithm parameters are invalid"));
    }
    if (algorithm->first == "2.16.840.1.101.3.4.2.1") {
        return crypto::digest_kind::sha256;
    }
    if (algorithm->first == "2.16.840.1.101.3.4.2.2") {
        return crypto::digest_kind::sha384;
    }
    return std::unexpected(unsupported("unsupported digest algorithm " + algorithm->first));
}

auto explicit_value(const element& value, std::uint32_t number, std::string_view name)
    -> result<element> {
    if (value.identifier != tag{tag_class::context_specific, number, true}) {
        return std::unexpected(corrupt(std::string(name) + " has the wrong DER tag"));
    }
    reader contents(value.content);
    auto output = contents.single();
    if (!output) {
        return std::unexpected(corrupt(std::string(name) + " is invalid"));
    }
    return *output;
}

auto parse_pss(const std::optional<element>& parameters) -> result<crypto::signature_parameters> {
    if (!parameters || parameters->identifier != sequence) {
        return std::unexpected(corrupt("RSA-PSS parameters are required"));
    }
    crypto::signature_parameters output{
        .kind = crypto::signature_kind::rsa_pss,
        .digest = crypto::digest_kind::sha256,
        .pss_salt_length = 20,
    };
    crypto::digest_kind mask_digest = crypto::digest_kind::sha256;
    reader fields(parameters->content);
    while (!fields.empty()) {
        auto field = next_required(fields, "RSA-PSS parameter");
        if (!field) {
            return std::unexpected(field.error());
        }
        if (field->identifier == tag{tag_class::context_specific, 0, true}) {
            auto value = explicit_value(*field, 0, "RSA-PSS hash algorithm");
            if (!value) {
                return std::unexpected(value.error());
            }
            auto parsed = digest_algorithm(*value);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            output.digest = *parsed;
        } else if (field->identifier == tag{tag_class::context_specific, 1, true}) {
            auto value = explicit_value(*field, 1, "RSA-PSS mask algorithm");
            if (!value) {
                return std::unexpected(value.error());
            }
            auto mask = algorithm_identifier(*value);
            if (!mask || mask->first != "1.2.840.113549.1.1.8" || !mask->second) {
                return std::unexpected(unsupported("unsupported RSA-PSS mask algorithm"));
            }
            auto parsed = digest_algorithm(*mask->second);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
            mask_digest = *parsed;
        } else if (field->identifier == tag{tag_class::context_specific, 2, true}) {
            auto value = explicit_value(*field, 2, "RSA-PSS salt length");
            if (!value) {
                return std::unexpected(value.error());
            }
            auto bytes = asn1::unsigned_integer(*value);
            if (!bytes || bytes->size() > 2) {
                return std::unexpected(corrupt("RSA-PSS salt length is invalid"));
            }
            output.pss_salt_length = 0;
            for (const std::byte byte : *bytes) {
                output.pss_salt_length =
                    output.pss_salt_length * 256 + std::to_integer<std::uint8_t>(byte);
            }
        } else if (field->identifier == tag{tag_class::context_specific, 3, true}) {
            auto value = explicit_value(*field, 3, "RSA-PSS trailer field");
            if (!value) {
                return std::unexpected(value.error());
            }
            auto bytes = asn1::unsigned_integer(*value);
            if (!bytes || bytes->size() != 1 || bytes->front() != std::byte{1}) {
                return std::unexpected(unsupported("unsupported RSA-PSS trailer field"));
            }
        } else {
            return std::unexpected(corrupt("unknown RSA-PSS parameter"));
        }
    }
    const std::size_t digest_size = output.digest == crypto::digest_kind::sha256 ? 32 : 48;
    if (mask_digest != output.digest || output.pss_salt_length != digest_size) {
        return std::unexpected(unsupported("RSA-PSS requires matching MGF1 and digest-size salt"));
    }
    return output;
}

auto parse_public_key(const element& value) -> result<crypto::public_key> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("subject public key info is invalid"));
    }
    reader fields(value.content);
    auto algorithm_element = next_required(fields, "public key algorithm");
    auto bits = next_required(fields, "subject public key");
    if (!algorithm_element || !bits || !fields.empty() || bits->identifier != bit_string_tag ||
        bits->content.empty() || bits->content.front() != std::byte{0}) {
        return std::unexpected(corrupt("subject public key info is invalid"));
    }
    auto algorithm = algorithm_identifier(*algorithm_element);
    if (!algorithm) {
        return std::unexpected(algorithm.error());
    }
    const auto key_bytes = bits->content.subspan(1);
    if (algorithm->first == "1.2.840.113549.1.1.1") {
        if (!algorithm->second ||
            algorithm->second->identifier != tag{tag_class::universal, 5, false} ||
            !algorithm->second->content.empty()) {
            return std::unexpected(corrupt("RSA public key parameters are invalid"));
        }
        reader key_reader(key_bytes);
        auto key_sequence = key_reader.single();
        if (!key_sequence || key_sequence->identifier != sequence) {
            return std::unexpected(corrupt("RSA public key is invalid"));
        }
        reader key_fields(key_sequence->content);
        auto modulus_element = next_required(key_fields, "RSA modulus");
        auto exponent_element = next_required(key_fields, "RSA exponent");
        if (!modulus_element || !exponent_element || !key_fields.empty()) {
            return std::unexpected(corrupt("RSA public key fields are invalid"));
        }
        auto modulus = asn1::unsigned_integer(*modulus_element);
        auto exponent = asn1::unsigned_integer(*exponent_element);
        if (!modulus || !exponent) {
            return std::unexpected(corrupt("RSA public key integers are invalid"));
        }
        return crypto::public_key{
            .kind = crypto::signature_kind::rsa_pkcs1,
            .first = copy(*modulus),
            .second = copy(*exponent),
        };
    }
    if (algorithm->first == "1.2.840.10045.2.1") {
        if (!algorithm->second) {
            return std::unexpected(corrupt("EC public key has no named curve"));
        }
        auto curve_oid = oid(*algorithm->second);
        if (!curve_oid) {
            return std::unexpected(curve_oid.error());
        }
        crypto::curve_kind curve = crypto::curve_kind::none;
        std::size_t coordinate_size = 0;
        if (*curve_oid == "1.2.840.10045.3.1.7") {
            curve = crypto::curve_kind::p256;
            coordinate_size = 32;
        } else if (*curve_oid == "1.3.132.0.34") {
            curve = crypto::curve_kind::p384;
            coordinate_size = 48;
        } else {
            return std::unexpected(unsupported("unsupported EC named curve " + *curve_oid));
        }
        if (key_bytes.size() != 1 + coordinate_size * 2 || key_bytes.front() != std::byte{4}) {
            return std::unexpected(corrupt("EC public key must be an uncompressed point"));
        }
        return crypto::public_key{
            .kind = crypto::signature_kind::ecdsa,
            .curve = curve,
            .first = copy(key_bytes.subspan(1, coordinate_size)),
            .second = copy(key_bytes.subspan(1 + coordinate_size, coordinate_size)),
        };
    }
    return std::unexpected(unsupported("unsupported public key algorithm " + algorithm->first));
}

auto parse_boolean(reader& fields, bool default_value) -> result<bool> {
    auto candidate = fields.next();
    if (!candidate) {
        return std::unexpected(candidate.error());
    }
    if (!*candidate || (*candidate)->identifier != boolean_tag) {
        return default_value;
    }
    if ((*candidate)->content.size() != 1 || ((*candidate)->content.front() != std::byte{0} &&
                                              (*candidate)->content.front() != std::byte{0xff})) {
        return std::unexpected(corrupt("BOOLEAN is not canonical DER"));
    }
    return (*candidate)->content.front() == std::byte{0xff};
}

struct extension_state {
    bool basic_constraints_seen = false;
    bool key_usage_seen = false;
    bool eku_seen = false;
};

auto parse_basic_constraints(detail::parsed_certificate& output,
                             const element& contents,
                             extension_state& state) -> result<void> {
    if (state.basic_constraints_seen) {
        return std::unexpected(corrupt("duplicate basic constraints extension"));
    }
    state.basic_constraints_seen = true;
    reader encoded(contents.content);
    auto value = encoded.single();
    if (!value || value->identifier != sequence) {
        return std::unexpected(corrupt("basic constraints extension is invalid"));
    }
    reader fields(value->content);
    auto first = fields.next();
    if (!first) {
        return std::unexpected(first.error());
    }
    if (*first && (*first)->identifier == boolean_tag) {
        if ((*first)->content.size() != 1 || (*first)->content.front() != std::byte{0xff}) {
            return std::unexpected(corrupt("basic constraints CA value is not canonical"));
        }
        output.summary.certificate_authority = true;
    }
    return {};
}

auto parse_key_usage(detail::parsed_certificate& output,
                     const element& contents,
                     extension_state& state) -> result<void> {
    if (state.key_usage_seen) {
        return std::unexpected(corrupt("duplicate key usage extension"));
    }
    state.key_usage_seen = true;
    reader encoded(contents.content);
    auto value = encoded.single();
    if (!value || value->identifier != bit_string_tag || value->content.size() < 2 ||
        std::to_integer<std::uint8_t>(value->content.front()) > 7) {
        return std::unexpected(corrupt("key usage extension is invalid"));
    }
    const auto bits = value->content.subspan(1);
    output.summary.digital_signature = (std::to_integer<std::uint8_t>(bits.front()) & 0x80) != 0;
    output.summary.certificate_signing =
        bits.size() > 0 && (std::to_integer<std::uint8_t>(bits.front()) & 0x04) != 0;
    return {};
}

auto parse_extended_key_usage(detail::parsed_certificate& output,
                              const element& contents,
                              bool critical,
                              extension_state& state) -> result<void> {
    if (state.eku_seen) {
        return std::unexpected(corrupt("duplicate extended key usage extension"));
    }
    state.eku_seen = true;
    reader encoded(contents.content);
    auto value = encoded.single();
    if (!value || value->identifier != sequence) {
        return std::unexpected(corrupt("extended key usage extension is invalid"));
    }
    reader purposes(value->content);
    std::size_t count = 0;
    while (!purposes.empty()) {
        auto purpose_element = next_required(purposes, "extended key purpose");
        if (!purpose_element) {
            return std::unexpected(purpose_element.error());
        }
        auto purpose_oid = oid(*purpose_element);
        if (!purpose_oid) {
            return std::unexpected(purpose_oid.error());
        }
        ++count;
        if (*purpose_oid == "1.3.6.1.5.5.7.3.8") {
            output.summary.timestamp_signing = true;
        }
    }
    output.timestamp_eku_critical = critical;
    if (output.summary.timestamp_signing && count != 1) {
        return std::unexpected(corrupt("timestamp signing EKU must be the only purpose"));
    }
    return {};
}

auto recognized_noncritical(std::string_view identifier) -> bool {
    constexpr std::array recognized{
        "2.5.29.14",
        "2.5.29.17",
        "2.5.29.31",
        "2.5.29.32",
        "2.5.29.35",
        "1.3.6.1.5.5.7.1.1",
    };
    return std::ranges::find(recognized, identifier) != recognized.end();
}

auto parse_extension(detail::parsed_certificate& output,
                     const element& value,
                     extension_state& state) -> result<void> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("certificate extension is invalid"));
    }
    reader fields(value.content);
    auto identifier_element = next_required(fields, "extension identifier");
    if (!identifier_element) {
        return std::unexpected(identifier_element.error());
    }
    auto identifier = oid(*identifier_element);
    if (!identifier) {
        return std::unexpected(identifier.error());
    }
    bool critical = false;
    auto next = fields.next();
    if (!next) {
        return std::unexpected(next.error());
    }
    if (*next && (*next)->identifier == boolean_tag) {
        if ((*next)->content.size() != 1 || (*next)->content.front() != std::byte{0xff}) {
            return std::unexpected(corrupt("extension critical flag is not canonical"));
        }
        critical = true;
        next = fields.next();
        if (!next) {
            return std::unexpected(next.error());
        }
    }
    if (!*next || (*next)->identifier != octet_string_tag || !fields.empty()) {
        return std::unexpected(corrupt("certificate extension value is invalid"));
    }
    if (*identifier == "2.5.29.19") {
        return parse_basic_constraints(output, **next, state);
    }
    if (*identifier == "2.5.29.15") {
        return parse_key_usage(output, **next, state);
    }
    if (*identifier == "2.5.29.37") {
        return parse_extended_key_usage(output, **next, critical, state);
    }
    if (critical || !recognized_noncritical(*identifier)) {
        if (critical) {
            return std::unexpected(
                unsupported("unknown critical certificate extension " + *identifier));
        }
    }
    return {};
}

auto parse_extensions(detail::parsed_certificate& output, const element& value) -> result<void> {
    auto explicit_extensions = explicit_value(value, 3, "certificate extensions");
    if (!explicit_extensions || explicit_extensions->identifier != sequence) {
        return std::unexpected(corrupt("certificate extensions are invalid"));
    }
    extension_state state;
    reader extensions(explicit_extensions->content);
    while (!extensions.empty()) {
        auto extension = next_required(extensions, "certificate extension");
        if (!extension) {
            return std::unexpected(extension.error());
        }
        auto parsed = parse_extension(output, *extension, state);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
    }
    return {};
}

auto parse_tbs(detail::parsed_certificate& output, const element& value) -> result<void> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("TBSCertificate is invalid"));
    }
    output.tbs = copy(value.encoded);
    reader fields(value.content);
    auto version_wrapper = next_required(fields, "certificate version");
    if (!version_wrapper ||
        version_wrapper->identifier != tag{tag_class::context_specific, 0, true}) {
        return std::unexpected(unsupported("only X.509 version 3 certificates are supported"));
    }
    auto version = explicit_value(*version_wrapper, 0, "certificate version");
    auto version_bytes = version ? asn1::unsigned_integer(*version)
                                 : result<std::span<const std::byte>>(
                                       std::unexpected(corrupt("certificate version is invalid")));
    if (!version_bytes || version_bytes->size() != 1 || version_bytes->front() != std::byte{2}) {
        return std::unexpected(unsupported("only X.509 version 3 certificates are supported"));
    }
    auto serial_element = next_required(fields, "certificate serial number");
    auto signature_algorithm = next_required(fields, "certificate signature algorithm");
    auto issuer = next_required(fields, "certificate issuer");
    auto validity = next_required(fields, "certificate validity");
    auto subject = next_required(fields, "certificate subject");
    auto public_key = next_required(fields, "certificate public key");
    if (!serial_element || !signature_algorithm || !issuer || !validity || !subject ||
        !public_key) {
        return std::unexpected(corrupt("certificate fields are incomplete"));
    }
    auto serial = asn1::unsigned_integer(*serial_element);
    if (!serial || serial->empty() || serial->size() > 20) {
        return std::unexpected(corrupt("certificate serial number is invalid"));
    }
    output.serial = copy(*serial);
    auto parsed_signature = detail::parse_signature_algorithm(*signature_algorithm);
    if (!parsed_signature) {
        return std::unexpected(parsed_signature.error());
    }
    output.signature_parameters = *parsed_signature;
    auto issuer_name = name(*issuer);
    auto subject_name = name(*subject);
    if (!issuer_name || !subject_name) {
        return std::unexpected(issuer_name ? subject_name.error() : issuer_name.error());
    }
    output.summary.issuer = *issuer_name;
    output.summary.subject = *subject_name;
    output.issuer_der = copy(issuer->encoded);
    output.subject_der = copy(subject->encoded);
    if (validity->identifier != sequence) {
        return std::unexpected(corrupt("certificate validity is invalid"));
    }
    reader validity_fields(validity->content);
    auto not_before = next_required(validity_fields, "notBefore");
    auto not_after = next_required(validity_fields, "notAfter");
    if (!not_before || !not_after || !validity_fields.empty()) {
        return std::unexpected(corrupt("certificate validity fields are invalid"));
    }
    auto parsed_not_before = parse_time(*not_before);
    auto parsed_not_after = parse_time(*not_after);
    if (!parsed_not_before || !parsed_not_after || *parsed_not_before > *parsed_not_after) {
        return std::unexpected(corrupt("certificate validity interval is invalid"));
    }
    output.summary.not_before = *parsed_not_before;
    output.summary.not_after = *parsed_not_after;
    auto parsed_key = parse_public_key(*public_key);
    if (!parsed_key) {
        return std::unexpected(parsed_key.error());
    }
    output.public_key = std::move(*parsed_key);

    while (!fields.empty()) {
        auto optional = next_required(fields, "optional certificate field");
        if (!optional) {
            return std::unexpected(optional.error());
        }
        if (optional->identifier == tag{tag_class::context_specific, 3, true}) {
            auto parsed = parse_extensions(output, *optional);
            if (!parsed) {
                return std::unexpected(parsed.error());
            }
        } else {
            return std::unexpected(unsupported("issuer and subject unique IDs are unsupported"));
        }
    }
    return {};
}

auto decode_base64(std::string_view input) -> result<std::vector<std::byte>> {
    constexpr std::string_view alphabet =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::vector<std::byte> output;
    std::uint32_t accumulator = 0;
    unsigned int bits = 0;
    bool padding = false;
    for (const char character : input) {
        if (std::isspace(static_cast<unsigned char>(character)) != 0) {
            continue;
        }
        if (character == '=') {
            padding = true;
            continue;
        }
        if (padding) {
            return std::unexpected(corrupt("PEM has data after padding"));
        }
        const std::size_t value = alphabet.find(character);
        if (value == std::string_view::npos) {
            return std::unexpected(corrupt("PEM contains invalid base64"));
        }
        accumulator = (accumulator << 6) | static_cast<std::uint32_t>(value);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            output.push_back(static_cast<std::byte>((accumulator >> bits) & 0xff));
        }
    }
    if (output.empty() || bits >= 6 || (accumulator & ((1U << bits) - 1)) != 0) {
        return std::unexpected(corrupt("PEM base64 is incomplete"));
    }
    return output;
}

auto valid_at(const detail::parsed_certificate& value, std::chrono::sys_seconds at) -> bool {
    return value.summary.not_before <= at && at <= value.summary.not_after;
}

auto same_bytes(std::span<const std::byte> left, std::span<const std::byte> right) -> bool {
    return std::ranges::equal(left, right);
}

auto build_path(const detail::parsed_certificate& leaf,
                std::span<const detail::parsed_certificate> intermediates,
                std::span<const detail::parsed_certificate> anchors,
                std::chrono::sys_seconds at) -> result<std::vector<certificate>> {
    std::vector<const detail::parsed_certificate*> path{&leaf};
    while (path.size() <= 8) {
        const detail::parsed_certificate& current = *path.back();
        if (!valid_at(current, at)) {
            return std::unexpected(corrupt("certificate is not valid at the timestamp time: " +
                                           current.summary.subject));
        }
        for (const auto& anchor : anchors) {
            if (same_bytes(current.summary.der, anchor.summary.der)) {
                std::vector<certificate> output;
                for (const auto* item : path) {
                    output.push_back(item->summary);
                }
                return output;
            }
            if (!same_bytes(current.issuer_der, anchor.subject_der)) {
                continue;
            }
            if (!valid_at(anchor, at)) {
                return std::unexpected(corrupt("trust anchor is not valid at the timestamp time"));
            }
            auto verified = crypto::verify_signature(
                anchor.public_key, current.signature_parameters, current.tbs, current.signature);
            if (!verified) {
                return std::unexpected(
                    corrupt("certificate signature does not verify to trust anchor"));
            }
            std::vector<certificate> output;
            for (const auto* item : path) {
                output.push_back(item->summary);
            }
            output.push_back(anchor.summary);
            return output;
        }

        const detail::parsed_certificate* issuer = nullptr;
        for (const auto& candidate : intermediates) {
            if (same_bytes(current.issuer_der, candidate.subject_der) &&
                std::ranges::find(path, &candidate) == path.end()) {
                issuer = &candidate;
                break;
            }
        }
        if (issuer == nullptr || !issuer->summary.certificate_authority ||
            !issuer->summary.certificate_signing) {
            return std::unexpected(
                error{errc::unauthorized,
                      "certificate path does not reach a configured trust anchor",
                      "X.509 path"});
        }
        auto verified = crypto::verify_signature(
            issuer->public_key, current.signature_parameters, current.tbs, current.signature);
        if (!verified) {
            return std::unexpected(corrupt("certificate signature is invalid"));
        }
        path.push_back(issuer);
    }
    return std::unexpected(corrupt("certificate path is too deep"));
}

} // namespace

namespace detail {

auto parse_signature_algorithm(const element& value) -> result<crypto::signature_parameters> {
    auto algorithm = algorithm_identifier(value);
    if (!algorithm) {
        return std::unexpected(algorithm.error());
    }
    if (algorithm->first == "1.2.840.113549.1.1.11" ||
        algorithm->first == "1.2.840.113549.1.1.12") {
        if (!algorithm->second ||
            algorithm->second->identifier != tag{tag_class::universal, 5, false} ||
            !algorithm->second->content.empty()) {
            return std::unexpected(corrupt("RSA signature parameters are invalid"));
        }
        return crypto::signature_parameters{
            .kind = crypto::signature_kind::rsa_pkcs1,
            .digest = algorithm->first.ends_with("11") ? crypto::digest_kind::sha256
                                                       : crypto::digest_kind::sha384,
        };
    }
    if (algorithm->first == "1.2.840.113549.1.1.10") {
        return parse_pss(algorithm->second);
    }
    if (algorithm->first == "1.2.840.10045.4.3.2" || algorithm->first == "1.2.840.10045.4.3.3") {
        if (algorithm->second) {
            return std::unexpected(corrupt("ECDSA signature parameters must be absent"));
        }
        return crypto::signature_parameters{
            .kind = crypto::signature_kind::ecdsa,
            .digest = algorithm->first.ends_with("2") ? crypto::digest_kind::sha256
                                                      : crypto::digest_kind::sha384,
        };
    }
    return std::unexpected(unsupported("unsupported signature algorithm " + algorithm->first));
}

auto parse(std::span<const std::byte> encoded) -> result<parsed_certificate> {
    reader input(encoded);
    auto certificate_sequence = input.single();
    if (!certificate_sequence || certificate_sequence->identifier != sequence) {
        return std::unexpected(corrupt("certificate is not one DER sequence"));
    }
    reader fields(certificate_sequence->content);
    auto tbs = next_required(fields, "TBSCertificate");
    auto outer_algorithm = next_required(fields, "certificate signature algorithm");
    auto signature = next_required(fields, "certificate signature");
    if (!tbs || !outer_algorithm || !signature || !fields.empty() ||
        signature->identifier != bit_string_tag || signature->content.empty() ||
        signature->content.front() != std::byte{0}) {
        return std::unexpected(corrupt("certificate outer fields are invalid"));
    }
    parsed_certificate output;
    output.summary.der = copy(encoded);
    auto parsed_tbs = parse_tbs(output, *tbs);
    if (!parsed_tbs) {
        return std::unexpected(parsed_tbs.error());
    }
    auto parsed_outer = parse_signature_algorithm(*outer_algorithm);
    if (!parsed_outer) {
        return std::unexpected(parsed_outer.error());
    }
    if (parsed_outer->kind != output.signature_parameters.kind ||
        parsed_outer->digest != output.signature_parameters.digest ||
        parsed_outer->pss_salt_length != output.signature_parameters.pss_salt_length) {
        return std::unexpected(corrupt("inner and outer certificate signature algorithms differ"));
    }
    output.signature = copy(signature->content.subspan(1));
    return output;
}

} // namespace detail

auto trust_anchor_set::add_der(std::span<const std::byte> encoded) -> result<void> {
    auto parsed = detail::parse(encoded);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (!parsed->summary.certificate_authority || !parsed->summary.certificate_signing) {
        return std::unexpected(corrupt("trust anchor is not a certificate-signing CA"));
    }
    if (std::ranges::none_of(certificates_,
                             [&](const auto& existing) { return same_bytes(existing, encoded); })) {
        certificates_.push_back(copy(encoded));
    }
    return {};
}

auto trust_anchor_set::add_pem(std::string_view encoded) -> result<void> {
    constexpr std::string_view begin = "-----BEGIN CERTIFICATE-----";
    constexpr std::string_view end = "-----END CERTIFICATE-----";
    std::size_t offset = 0;
    std::size_t added = 0;
    while (true) {
        const std::size_t start = encoded.find(begin, offset);
        if (start == std::string_view::npos) {
            break;
        }
        const std::size_t content_start = start + begin.size();
        const std::size_t finish = encoded.find(end, content_start);
        if (finish == std::string_view::npos) {
            return std::unexpected(corrupt("PEM certificate has no end marker"));
        }
        auto der = decode_base64(encoded.substr(content_start, finish - content_start));
        if (!der) {
            return std::unexpected(der.error());
        }
        auto inserted = add_der(*der);
        if (!inserted) {
            return std::unexpected(inserted.error());
        }
        ++added;
        offset = finish + end.size();
    }
    if (added == 0) {
        return std::unexpected(corrupt("trust value contains no PEM certificate"));
    }
    return {};
}

auto trust_anchor_set::empty() const noexcept -> bool {
    return certificates_.empty();
}

auto trust_anchor_set::certificates() const noexcept -> const std::vector<std::vector<std::byte>>& {
    return certificates_;
}

auto parse_certificate(std::span<const std::byte> encoded) -> result<certificate> {
    auto parsed = detail::parse(encoded);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::move(parsed->summary);
}

auto validate_path(const certificate& leaf,
                   std::span<const certificate> intermediates,
                   const trust_anchor_set& trust,
                   std::chrono::sys_seconds at,
                   purpose required_purpose) -> result<std::vector<certificate>> {
    if (trust.empty()) {
        return std::unexpected(
            error{errc::unauthorized, "no TSA trust anchors are configured", "X.509 path"});
    }
    auto parsed_leaf = detail::parse(leaf.der);
    if (!parsed_leaf) {
        return std::unexpected(parsed_leaf.error());
    }
    if (required_purpose == purpose::timestamp_signing &&
        (!parsed_leaf->summary.digital_signature || !parsed_leaf->summary.timestamp_signing ||
         !parsed_leaf->timestamp_eku_critical)) {
        return std::unexpected(error{errc::unauthorized,
                                     "TSA certificate lacks critical id-kp-timeStamping purpose",
                                     "X.509 path"});
    }
    std::vector<detail::parsed_certificate> parsed_intermediates;
    for (const auto& candidate : intermediates) {
        auto parsed = detail::parse(candidate.der);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        parsed_intermediates.push_back(std::move(*parsed));
    }
    std::vector<detail::parsed_certificate> parsed_anchors;
    for (const auto& encoded : trust.certificates()) {
        auto parsed = detail::parse(encoded);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        parsed_anchors.push_back(std::move(*parsed));
    }
    return build_path(*parsed_leaf, parsed_intermediates, parsed_anchors, at);
}

} // namespace liquid::x509
