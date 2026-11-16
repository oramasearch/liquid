// SPDX-License-Identifier: MIT

// The data-file readers under src/formats/ (liquid #463): HDF5, Parquet, SQLite and PDF, each
// through codec.hpp's driver. One selector byte picks the codec, in that order.

#include <cstddef>
#include <cstdint>

#include "codec.hpp"
#include "fuzz.hpp"

namespace detail = liquid::formats::detail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const auto codecs = liquid::fuzz::link_codecs({detail::hdf5_module(),
                                                          detail::parquet_module(),
                                                          detail::sqlite_module(),
                                                          detail::pdf_module()});
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), codecs.size());
    if (!input) {
        return 0;
    }
    liquid::fuzz::exercise(codecs[input->decoder], input->payload);
    return 0;
}
