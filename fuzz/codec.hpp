#pragma once

// Shared driver for the format-codec harnesses (archive, binary, data, model; liquid #463).
//
// Each built-in codec is linked once through format_codec::linked, the path the registry uses, so
// the input crosses the same C ABI and CBOR boundary a registered codec's does. The driver runs
// recognise, then unpack with small limits, then canonicalise, diff and repack on the tree it
// got. Results are ignored: the sanitizers and libFuzzer's own limits report the defects.

#include <cstddef>
#include <initializer_list>
#include <span>
#include <utility>
#include <vector>

#include "formats/builtin.hpp"
#include "liquid/format.hpp"

namespace liquid::fuzz {

[[nodiscard]] inline auto link_codecs(std::initializer_list<const liquid_format_module_v1*> modules)
    -> std::vector<format_codec> {
    auto output = std::vector<format_codec>{};
    for (const auto* module : modules) {
        auto codec = format_codec::linked(module);
        if (codec) {
            output.push_back(std::move(*codec));
        }
    }
    return output;
}

[[nodiscard]] inline auto small_limits() -> format_limits {
    return {.maximum_depth = 4, .byte_budget = 16ULL * 1024ULL * 1024ULL, .maximum_entries = 4096};
}

inline void exercise(const format_codec& codec, std::span<const std::byte> payload) {
    (void)codec.recognise(payload);
    const auto tree = codec.unpack(payload, small_limits());
    if (!tree) {
        return;
    }
    const auto canonical = codec.canonicalise(*tree);
    (void)codec.diff(*tree, canonical ? *canonical : *tree);
    (void)codec.repack(*tree);
}

// The whole registry, as `liquid` reads a file it has not classified: recognise by magic and
// unpack, descending into nested containers.
inline void exercise_registry(std::span<const std::byte> payload) {
    (void)format_registry::global().unpack(payload, {}, small_limits());
}

} // namespace liquid::fuzz
