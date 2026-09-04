#include "liquid/object.hpp"

#include "identity/taap.hpp"
#include "objects/records.hpp"

namespace liquid {

result<bool> verify(const attestation& value) {
    auto body = objects::encode_attestation_body(value);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto signed_value = identity_detail::parse_signature(value.signature);
    if (!signed_value) {
        return std::unexpected(signed_value.error());
    }
    return verify(value.issuer, "attestation", *body, *signed_value);
}

} // namespace liquid
