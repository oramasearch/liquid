// The sync protocol's wire framing: the metadata document, the object list and pack frames that a
// peer sends during `liquid sync`, `clone` and `fetch`.

#include <cstddef>
#include <cstdint>

#include "fuzz.hpp"
#include "sync/codec.hpp"

namespace codec = liquid::sync_detail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 3);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0: {
        const auto decoded = codec::decode_metadata(input->payload);
        if (decoded) {
            (void)codec::encode_metadata(*decoded);
        }
        break;
    }
    case 1: {
        const auto decoded = codec::decode_object_list(input->payload);
        if (decoded) {
            (void)codec::encode_object_list(*decoded);
        }
        break;
    }
    default:
        (void)codec::decode_pack_frames(input->payload);
        break;
    }
    return 0;
}
