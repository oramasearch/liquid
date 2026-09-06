#include "liquid/attestation.hpp"

#include <chrono>
#include <string_view>
#include <utility>

#include "liquid/store.hpp"
#include "objects/records.hpp"

namespace liquid {

runtime::task<digest> attest_image(std::string image_reference,
                                   digest tree,
                                   std::optional<digest> result,
                                   std::filesystem::path store_directory) {
    const auto separator = image_reference.rfind('@');
    auto subject =
        separator == std::string::npos
            ? liquid::result<digest>{std::unexpected(
                  error{errc::invalid_argument, "image not pinned by digest", image_reference})}
            : parse_digest(std::string_view{image_reference}.substr(separator + 1));
    if (!subject || separator == 0 || subject->alg != algorithm::sha256) {
        co_return std::unexpected(
            error{errc::invalid_argument, "image not pinned by digest", image_reference});
    }
    auto statement = attestation{
        .subject = *subject,
        .profile = "image_provenance/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .subject_kind = attestation_subject_kind::artifact,
        .attributes =
            {
                {"image", std::move(image_reference)},
                {"tree", to_string(tree)},
            },
    };
    if (result) {
        statement.attributes.push_back({"result", to_string(*result)});
    }
    auto signer = co_await identity_detail::local_signer();
    if (!signer) {
        co_return std::unexpected(signer.error());
    }
    statement.issuer = signer->identity();
    auto body = objects::encode_attestation_body(statement);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_value = co_await signer->sign("attestation", std::move(*body));
    if (!signed_value) {
        co_return std::unexpected(signed_value.error());
    }
    auto signature = encode_attestation_signature(*signed_value);
    if (!signature) {
        co_return std::unexpected(signature.error());
    }
    statement.signature = std::move(*signature);
    auto encoded = encode_attestation(statement);
    if (!encoded) {
        co_return std::unexpected(encoded.error());
    }
    auto destination = co_await store::open(std::move(store_directory));
    if (!destination) {
        co_return std::unexpected(destination.error());
    }
    co_return co_await destination->put(std::move(*encoded));
}

} // namespace liquid
