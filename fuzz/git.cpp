// Git objects as `liquid import` and the Git bridge read them: a loose object's header and its
// identifier check, commit and tree payloads, and pack deltas.

#include <cstddef>
#include <cstdint>
#include <vector>

#include "fuzz.hpp"
#include "git/object_store.hpp"
#include "git/sha1.hpp"

namespace git = liquid::git_detail;

namespace {

auto payload_object(git::object_kind kind, std::span<const std::byte> payload) -> git::object {
    return {kind, git::sha1_hash(payload), {payload.begin(), payload.end()}};
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 4);
    if (!input) {
        return 0;
    }
    const auto payload = input->payload;
    switch (input->decoder) {
    case 0: {
        // The identifier is computed from the input, so the header and payload checks run.
        const auto object = git::parse_object(payload, git::sha1_hash(payload));
        if (object) {
            (void)git::parse_commit(*object);
            (void)git::parse_tree(*object);
        }
        break;
    }
    case 1:
        (void)git::parse_commit(payload_object(git::object_kind::commit, payload));
        break;
    case 2:
        (void)git::parse_tree(payload_object(git::object_kind::tree, payload));
        break;
    default: {
        const auto split = liquid::fuzz::parts(payload, 2);
        if (split.size() == 2) {
            (void)git::apply_delta(split[0], split[1]);
        }
        break;
    }
    }
    return 0;
}
