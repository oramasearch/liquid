// SPDX-License-Identifier: MIT

// The workers protocol (liquid #463): the scheduler's gRPC messages a worker or a client sends and
// the scheduler answers with (src/workers/protocol.cpp). One selector byte picks the message;
// each decoded message is encoded again.
//
//   0 submit request    3 heartbeat request    6 read-object request
//   1 worker request    4 complete request     7 read-object response
//   2 lease response    5 acknowledgement      8 scheduler status

#include <cstddef>
#include <cstdint>
#include <span>

#include "fuzz.hpp"
#include "workers/protocol.hpp"

namespace workers = liquid::workers;

namespace {

template <typename Message> void decode(std::span<const std::byte> input) {
    const auto message = workers::decode<Message>(input);
    if (message) {
        (void)workers::encode(*message);
    }
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 9);
    if (!input) {
        return 0;
    }
    const auto payload = input->payload;
    switch (input->decoder) {
    case 0:
        decode<workers::submit_request>(payload);
        break;
    case 1:
        decode<workers::worker_request>(payload);
        break;
    case 2:
        decode<workers::lease_response>(payload);
        break;
    case 3:
        decode<workers::heartbeat_request>(payload);
        break;
    case 4:
        decode<workers::complete_request>(payload);
        break;
    case 5:
        decode<workers::acknowledgement>(payload);
        break;
    case 6:
        decode<workers::read_object_request>(payload);
        break;
    case 7:
        decode<workers::read_object_response>(payload);
        break;
    default:
        decode<workers::scheduler_status>(payload);
        break;
    }
    return 0;
}
