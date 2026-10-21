// The object records: blobs, trees, changes, conflicts and the build records that share the store
// and travel between peers. One selector byte picks the record type.

#include <cstddef>
#include <cstdint>

#include "fuzz.hpp"
#include "objects/records.hpp"

namespace objects = liquid::objects;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 14);
    if (!input) {
        return 0;
    }
    const auto payload = input->payload;
    switch (input->decoder) {
    case 0: {
        const auto decoded = objects::decode_blob(payload);
        if (decoded) {
            (void)objects::encode(*decoded);
        }
        break;
    }
    case 1: {
        const auto decoded = objects::decode_tree(payload);
        if (decoded) {
            (void)objects::encode(*decoded);
        }
        break;
    }
    case 2:
        (void)objects::decode_conflict(payload);
        break;
    case 3: {
        const auto decoded = objects::decode_change(payload);
        if (decoded) {
            (void)objects::encode(*decoded);
        }
        break;
    }
    case 4:
        (void)objects::decode_repo(payload);
        break;
    case 5:
        (void)objects::decode_attestation(payload);
        break;
    case 6:
        (void)objects::decode_action(payload);
        break;
    case 7:
        (void)objects::decode_result(payload);
        break;
    case 8:
        (void)objects::decode_action_index_root(payload);
        break;
    case 9:
        (void)objects::decode_build_trace(payload);
        break;
    case 10:
        (void)objects::decode_sbom(payload);
        break;
    case 11:
        (void)objects::decode_abi(payload);
        break;
    case 12:
        (void)objects::decode_execution_schedule(payload);
        break;
    default:
        (void)objects::decode_change(payload, liquid::algorithm::sha256);
        break;
    }
    return 0;
}
