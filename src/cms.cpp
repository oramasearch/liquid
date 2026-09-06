#include "liquid/cms.hpp"

#include <algorithm>
#include <optional>
#include <utility>

#include "crypto/cms_internal.hpp"
#include "crypto/signature.hpp"
#include "crypto/x509_internal.hpp"
#include "liquid/asn1.hpp"

namespace liquid::cms {
namespace {

using asn1::element;
using asn1::reader;
using asn1::tag;
using asn1::tag_class;

constexpr tag sequence{tag_class::universal, 16, true};
constexpr tag set{tag_class::universal, 17, true};
constexpr tag octet_string{tag_class::universal, 4, false};

struct signer_info {
    std::vector<std::byte> issuer;
    std::vector<std::byte> serial;
    crypto::digest_kind digest = crypto::digest_kind::sha256;
    crypto::signature_parameters signature_parameters;
    std::vector<std::byte> signed_attributes;
    std::vector<std::byte> message_digest;
    std::string content_type;
    std::vector<std::byte> signature;
};

struct parsed_signed_data {
    std::vector<std::byte> content;
    std::string content_type;
    crypto::digest_kind digest = crypto::digest_kind::sha256;
    std::vector<x509::detail::parsed_certificate> certificates;
    signer_info signer;
};

auto corrupt(std::string message) -> error {
    return error{errc::corrupt, std::move(message), "CMS SignedData"};
}

auto unsupported(std::string message) -> error {
    return error{errc::unsupported, std::move(message), "CMS SignedData"};
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

auto oid(const element& value) -> result<std::string> {
    auto output = asn1::object_identifier(value);
    if (!output) {
        return std::unexpected(corrupt(output.error().message));
    }
    return output;
}

auto algorithm_identifier(const element& value)
    -> result<std::pair<std::string, std::optional<element>>> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("algorithm identifier is not a sequence"));
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
    if (algorithm->second &&
        (algorithm->second->identifier != tag{tag_class::universal, 5, false} ||
         !algorithm->second->content.empty())) {
        return std::unexpected(corrupt("digest algorithm parameters are invalid"));
    }
    if (algorithm->first == "2.16.840.1.101.3.4.2.1") {
        return crypto::digest_kind::sha256;
    }
    if (algorithm->first == "2.16.840.1.101.3.4.2.2") {
        return crypto::digest_kind::sha384;
    }
    return std::unexpected(unsupported("unsupported CMS digest algorithm " + algorithm->first));
}

auto validate_set_order(std::span<const std::byte> encoded) -> result<void> {
    reader values(encoded);
    std::optional<std::span<const std::byte>> previous;
    while (!values.empty()) {
        auto value = next_required(values, "SET value");
        if (!value) {
            return std::unexpected(value.error());
        }
        if (previous &&
            std::lexicographical_compare(
                value->encoded.begin(), value->encoded.end(), previous->begin(), previous->end())) {
            return std::unexpected(corrupt("SET values are not in DER order"));
        }
        previous = value->encoded;
    }
    return {};
}

auto single_attribute_value(const element& value) -> result<std::pair<std::string, element>> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("signed attribute is not a sequence"));
    }
    reader fields(value.content);
    auto identifier = next_required(fields, "signed attribute identifier");
    auto values = next_required(fields, "signed attribute values");
    if (!identifier || !values || !fields.empty() || values->identifier != set) {
        return std::unexpected(corrupt("signed attribute fields are invalid"));
    }
    auto ordered = validate_set_order(values->content);
    if (!ordered) {
        return std::unexpected(ordered.error());
    }
    auto identifier_text = oid(*identifier);
    if (!identifier_text) {
        return std::unexpected(identifier_text.error());
    }
    reader contents(values->content);
    auto content = contents.single();
    if (!content) {
        return std::unexpected(corrupt("signed attribute must contain one value"));
    }
    return std::pair{*identifier_text, *content};
}

auto parse_attributes(signer_info& output, const element& value) -> result<void> {
    if (value.identifier != tag{tag_class::context_specific, 0, true}) {
        return std::unexpected(corrupt("CMS signed attributes are required"));
    }
    auto ordered = validate_set_order(value.content);
    if (!ordered) {
        return std::unexpected(ordered.error());
    }
    output.signed_attributes = copy(value.encoded);
    output.signed_attributes.front() = std::byte{0x31};

    bool content_type_seen = false;
    bool message_digest_seen = false;
    reader attributes(value.content);
    while (!attributes.empty()) {
        auto attribute_element = next_required(attributes, "signed attribute");
        if (!attribute_element) {
            return std::unexpected(attribute_element.error());
        }
        auto attribute = single_attribute_value(*attribute_element);
        if (!attribute) {
            return std::unexpected(attribute.error());
        }
        if (attribute->first == "1.2.840.113549.1.9.3") {
            if (content_type_seen) {
                return std::unexpected(corrupt("duplicate contentType signed attribute"));
            }
            auto content_type = oid(attribute->second);
            if (!content_type) {
                return std::unexpected(content_type.error());
            }
            output.content_type = *content_type;
            content_type_seen = true;
        } else if (attribute->first == "1.2.840.113549.1.9.4") {
            if (message_digest_seen || attribute->second.identifier != octet_string) {
                return std::unexpected(corrupt("messageDigest signed attribute is invalid"));
            }
            output.message_digest = copy(attribute->second.content);
            message_digest_seen = true;
        }
    }
    if (!content_type_seen || !message_digest_seen) {
        return std::unexpected(corrupt("CMS signed attributes lack contentType or messageDigest"));
    }
    return {};
}

auto parse_cms_signature_algorithm(const element& value, crypto::digest_kind digest)
    -> result<crypto::signature_parameters> {
    auto algorithm = algorithm_identifier(value);
    if (!algorithm) {
        return std::unexpected(algorithm.error());
    }
    if (algorithm->first == "1.2.840.113549.1.1.1") {
        if (algorithm->second &&
            (algorithm->second->identifier != tag{tag_class::universal, 5, false} ||
             !algorithm->second->content.empty())) {
            return std::unexpected(corrupt("CMS RSA parameters are invalid"));
        }
        return crypto::signature_parameters{
            .kind = crypto::signature_kind::rsa_pkcs1,
            .digest = digest,
        };
    }
    auto parameters = x509::detail::parse_signature_algorithm(value);
    if (!parameters) {
        return std::unexpected(parameters.error());
    }
    if (parameters->digest != digest) {
        return std::unexpected(corrupt("CMS digest and signature algorithms differ"));
    }
    return parameters;
}

auto parse_signer(const element& value) -> result<signer_info> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("SignerInfo is not a sequence"));
    }
    reader fields(value.content);
    auto version = next_required(fields, "SignerInfo version");
    auto sid = next_required(fields, "SignerInfo identifier");
    auto digest = next_required(fields, "SignerInfo digest algorithm");
    auto attributes = next_required(fields, "SignerInfo signed attributes");
    auto signature_algorithm = next_required(fields, "SignerInfo signature algorithm");
    auto signature = next_required(fields, "SignerInfo signature");
    if (!version || !sid || !digest || !attributes || !signature_algorithm || !signature ||
        !fields.empty()) {
        return std::unexpected(corrupt("SignerInfo fields are invalid"));
    }
    auto version_bytes = asn1::unsigned_integer(*version);
    if (!version_bytes || version_bytes->size() != 1 || version_bytes->front() != std::byte{1}) {
        return std::unexpected(
            unsupported("only issuer-and-serial SignerInfo version 1 is supported"));
    }
    if (sid->identifier != sequence) {
        return std::unexpected(
            unsupported("only issuer-and-serial signer identifiers are supported"));
    }
    signer_info output;
    reader sid_fields(sid->content);
    auto issuer = next_required(sid_fields, "signer issuer");
    auto serial = next_required(sid_fields, "signer serial number");
    if (!issuer || !serial || !sid_fields.empty()) {
        return std::unexpected(corrupt("signer identifier is invalid"));
    }
    auto serial_bytes = asn1::unsigned_integer(*serial);
    if (!serial_bytes) {
        return std::unexpected(corrupt("signer serial number is invalid"));
    }
    output.issuer = copy(issuer->encoded);
    output.serial = copy(*serial_bytes);
    auto parsed_digest = digest_algorithm(*digest);
    if (!parsed_digest) {
        return std::unexpected(parsed_digest.error());
    }
    output.digest = *parsed_digest;
    auto parsed_attributes = parse_attributes(output, *attributes);
    if (!parsed_attributes) {
        return std::unexpected(parsed_attributes.error());
    }
    auto parsed_signature_algorithm =
        parse_cms_signature_algorithm(*signature_algorithm, output.digest);
    if (!parsed_signature_algorithm) {
        return std::unexpected(parsed_signature_algorithm.error());
    }
    output.signature_parameters = *parsed_signature_algorithm;
    if (signature->identifier != octet_string || signature->content.empty()) {
        return std::unexpected(corrupt("CMS signature is empty or invalid"));
    }
    output.signature = copy(signature->content);
    return output;
}

auto parse_certificates(const element& value)
    -> result<std::vector<x509::detail::parsed_certificate>> {
    if (value.identifier != tag{tag_class::context_specific, 0, true}) {
        return std::unexpected(corrupt("CMS certificates field is invalid"));
    }
    std::vector<x509::detail::parsed_certificate> output;
    reader certificates(value.content);
    while (!certificates.empty()) {
        auto certificate = next_required(certificates, "CMS certificate");
        if (!certificate || certificate->identifier != sequence) {
            return std::unexpected(corrupt("CMS certificate choice is unsupported"));
        }
        auto parsed = x509::detail::parse(certificate->encoded);
        if (!parsed) {
            return std::unexpected(parsed.error());
        }
        output.push_back(std::move(*parsed));
    }
    if (output.empty()) {
        return std::unexpected(corrupt("CMS SignedData contains no certificates"));
    }
    return output;
}

auto parse_encapsulated_content(const element& value)
    -> result<std::pair<std::string, std::vector<std::byte>>> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("encapsulated content info is invalid"));
    }
    reader fields(value.content);
    auto type = next_required(fields, "encapsulated content type");
    auto wrapper = next_required(fields, "encapsulated content");
    if (!type || !wrapper || !fields.empty() ||
        wrapper->identifier != tag{tag_class::context_specific, 0, true}) {
        return std::unexpected(corrupt("encapsulated content fields are invalid"));
    }
    auto type_oid = oid(*type);
    if (!type_oid) {
        return std::unexpected(type_oid.error());
    }
    reader wrapped(wrapper->content);
    auto contents = wrapped.single();
    if (!contents || contents->identifier != octet_string) {
        return std::unexpected(corrupt("encapsulated content must be one OCTET STRING"));
    }
    return std::pair{*type_oid, copy(contents->content)};
}

auto parse_signed_data(const element& value) -> result<parsed_signed_data> {
    if (value.identifier != sequence) {
        return std::unexpected(corrupt("SignedData is not a sequence"));
    }
    reader fields(value.content);
    auto version = next_required(fields, "SignedData version");
    auto digest_algorithms = next_required(fields, "SignedData digest algorithms");
    auto content_info = next_required(fields, "encapsulated content info");
    auto certificates = next_required(fields, "SignedData certificates");
    auto signer_infos = next_required(fields, "SignedData signer infos");
    if (!version || !digest_algorithms || !content_info || !certificates || !signer_infos ||
        !fields.empty()) {
        return std::unexpected(corrupt("SignedData fields are invalid"));
    }
    auto version_bytes = asn1::unsigned_integer(*version);
    if (!version_bytes || version_bytes->size() != 1 || version_bytes->front() != std::byte{3}) {
        return std::unexpected(unsupported("RFC 3161 SignedData must use version 3"));
    }
    if (digest_algorithms->identifier != set) {
        return std::unexpected(corrupt("SignedData digest algorithms are not a SET"));
    }
    auto ordered_digests = validate_set_order(digest_algorithms->content);
    if (!ordered_digests) {
        return std::unexpected(ordered_digests.error());
    }
    reader digest_values(digest_algorithms->content);
    auto digest_value = digest_values.single();
    if (!digest_value) {
        return std::unexpected(corrupt("SignedData must contain one digest algorithm"));
    }
    auto parsed_digest = digest_algorithm(*digest_value);
    if (!parsed_digest) {
        return std::unexpected(parsed_digest.error());
    }
    auto content = parse_encapsulated_content(*content_info);
    auto parsed_certificates = parse_certificates(*certificates);
    if (!content || !parsed_certificates) {
        return std::unexpected(content ? parsed_certificates.error() : content.error());
    }
    if (signer_infos->identifier != set) {
        return std::unexpected(corrupt("SignerInfos is not a SET"));
    }
    auto ordered_signers = validate_set_order(signer_infos->content);
    if (!ordered_signers) {
        return std::unexpected(ordered_signers.error());
    }
    reader signers(signer_infos->content);
    auto signer_value = signers.single();
    if (!signer_value) {
        return std::unexpected(corrupt("SignedData must contain one signer"));
    }
    auto signer = parse_signer(*signer_value);
    if (!signer) {
        return std::unexpected(signer.error());
    }
    if (signer->digest != *parsed_digest) {
        return std::unexpected(corrupt("SignedData and SignerInfo digest algorithms differ"));
    }
    return parsed_signed_data{
        .content = std::move(content->second),
        .content_type = std::move(content->first),
        .digest = *parsed_digest,
        .certificates = std::move(*parsed_certificates),
        .signer = std::move(*signer),
    };
}

auto parse_content_info(std::span<const std::byte> encoded) -> result<parsed_signed_data> {
    reader input(encoded);
    auto content_info = input.single();
    if (!content_info || content_info->identifier != sequence) {
        return std::unexpected(corrupt("ContentInfo is not one DER sequence"));
    }
    reader fields(content_info->content);
    auto content_type = next_required(fields, "ContentInfo type");
    auto content = next_required(fields, "ContentInfo content");
    if (!content_type || !content || !fields.empty() ||
        content->identifier != tag{tag_class::context_specific, 0, true}) {
        return std::unexpected(corrupt("ContentInfo fields are invalid"));
    }
    auto type_oid = oid(*content_type);
    if (!type_oid) {
        return std::unexpected(type_oid.error());
    }
    if (*type_oid != "1.2.840.113549.1.7.2") {
        return std::unexpected(unsupported("CMS content is not SignedData"));
    }
    reader wrapped(content->content);
    auto signed_data = wrapped.single();
    if (!signed_data) {
        return std::unexpected(corrupt("SignedData wrapper is invalid"));
    }
    return parse_signed_data(*signed_data);
}

auto find_signer(parsed_signed_data& value) -> result<std::size_t> {
    for (std::size_t index = 0; index < value.certificates.size(); ++index) {
        const auto& certificate = value.certificates[index];
        if (std::ranges::equal(certificate.issuer_der, value.signer.issuer) &&
            std::ranges::equal(certificate.serial, value.signer.serial)) {
            return index;
        }
    }
    return std::unexpected(corrupt("SignerInfo does not identify an embedded certificate"));
}

auto digest_name(crypto::digest_kind value) -> std::string {
    return value == crypto::digest_kind::sha256 ? "sha256" : "sha384";
}

auto signature_name(crypto::signature_kind value) -> std::string {
    switch (value) {
    case crypto::signature_kind::rsa_pkcs1:
        return "rsa-pkcs1-v1_5";
    case crypto::signature_kind::rsa_pss:
        return "rsa-pss";
    case crypto::signature_kind::ecdsa:
        return "ecdsa";
    }
    return "unknown";
}

} // namespace

namespace detail {

auto signed_content(std::span<const std::byte> encoded) -> result<std::vector<std::byte>> {
    auto parsed = parse_content_info(encoded);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    return std::move(parsed->content);
}

} // namespace detail

auto signed_data::verify(std::span<const std::byte> encoded,
                         const x509::trust_anchor_set& trust,
                         std::chrono::sys_seconds validation_time)
    -> result<signed_data_verification> {
    auto parsed = parse_content_info(encoded);
    if (!parsed) {
        return std::unexpected(parsed.error());
    }
    if (parsed->content_type != "1.2.840.113549.1.9.16.1.4" ||
        parsed->signer.content_type != parsed->content_type) {
        return std::unexpected(corrupt("CMS content type is not id-ct-TSTInfo"));
    }
    auto content_digest = crypto::digest(parsed->digest, parsed->content);
    if (!content_digest) {
        return std::unexpected(content_digest.error());
    }
    if (!std::ranges::equal(*content_digest, parsed->signer.message_digest)) {
        return std::unexpected(corrupt("CMS messageDigest does not match TSTInfo"));
    }
    auto signer_index = find_signer(*parsed);
    if (!signer_index) {
        return std::unexpected(signer_index.error());
    }
    const auto& signer = parsed->certificates[*signer_index];
    auto verified_signature = crypto::verify_signature(signer.public_key,
                                                       parsed->signer.signature_parameters,
                                                       parsed->signer.signed_attributes,
                                                       parsed->signer.signature);
    if (!verified_signature) {
        return std::unexpected(corrupt("CMS signer signature is invalid"));
    }
    std::vector<x509::certificate> intermediates;
    for (std::size_t index = 0; index < parsed->certificates.size(); ++index) {
        if (index != *signer_index) {
            intermediates.push_back(parsed->certificates[index].summary);
        }
    }
    auto path = x509::validate_path(
        signer.summary, intermediates, trust, validation_time, x509::purpose::timestamp_signing);
    if (!path) {
        return std::unexpected(path.error());
    }
    return signed_data_verification{
        .content = std::move(parsed->content),
        .content_type = std::move(parsed->content_type),
        .digest_algorithm = digest_name(parsed->digest),
        .signature_algorithm = signature_name(parsed->signer.signature_parameters.kind),
        .signer = signer.summary,
        .certificate_path = std::move(*path),
    };
}

} // namespace liquid::cms
