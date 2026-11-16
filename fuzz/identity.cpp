// The identity chain and the signature envelope: TAAP identity documents, their signatures, a
// chain of documents across key rotations, and scan attestations. Signatures are checked, so the
// verification code runs on whatever decodes.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fuzz.hpp"
#include "liquid/attestation.hpp"
#include "liquid/identity.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 4);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto document = liquid::decode_identity_document(input->payload);
        if (document) {
            (void)liquid::verify_document(*document);
            (void)liquid::encode_identity_document(*document);
        }
        break;
    }
    case 1:
        (void)liquid::decode_signature(input->payload);
        break;
    case 2: {
        const auto attestation = liquid::decode_attestation(input->payload);
        if (attestation) {
            (void)liquid::verify(*attestation);
            (void)liquid::verify_attestation(*attestation);
        }
        break;
    }
    default: {
        auto documents = std::vector<liquid::identity_document>{};
        for (const auto part : liquid::fuzz::parts(input->payload, 8)) {
            auto document = liquid::decode_identity_document(part);
            if (!document) {
                return 0;
            }
            documents.push_back(std::move(*document));
        }
        if (!documents.empty()) {
            (void)liquid::verify_chain(documents);
        }
        break;
    }
    }
    return 0;
}
