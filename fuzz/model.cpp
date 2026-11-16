// SPDX-License-Identifier: MIT

// The model-file readers in src/formats/model/ (liquid #463): Jupyter notebooks, safetensors and
// pickle, each through codec.hpp's driver. None executes a payload; the pickle reader walks the
// opcode stream. One selector byte picks the codec, in that order.

#include <cstddef>
#include <cstdint>

#include "codec.hpp"
#include "fuzz.hpp"

namespace detail = liquid::formats::detail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const auto codecs = liquid::fuzz::link_codecs(
        {detail::jupyter_module(), detail::safetensors_module(), detail::pickle_module()});
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), codecs.size());
    if (!input) {
        return 0;
    }
    liquid::fuzz::exercise(codecs[input->decoder], input->payload);
    return 0;
}
