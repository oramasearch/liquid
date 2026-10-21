// Drip decoding: the journal record of one captured write (crdt::decode_drip_record), and the
// signed drip body that history verification reads back from the store and from peers.

#include <cstddef>
#include <cstdint>

#include "crdt/journal.hpp"
#include "fuzz.hpp"
#include "repository/testing.hpp"

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 2);
    if (!input) {
        return 0;
    }
    if (input->decoder == 0) {
        (void)liquid::crdt::decode_drip_record(input->payload);
    } else {
        (void)liquid::repository_detail::testing::decode_drip_body(input->payload);
    }
    return 0;
}
