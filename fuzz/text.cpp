// SPDX-License-Identifier: MIT

// The strict structured-text codecs in src/formats/ (liquid #463): JSON (the reader the TUF client,
// admission, Jupyter notebooks and repository metadata use) and .env files. Each is parsed, written
// and parsed again: a JSON value must come back equal, a .env file with the same lines and keys.
//
//   0  JSON
//   1  .env

#include <cstddef>
#include <cstdint>

#include "formats/dotenv.hpp"
#include "formats/json.hpp"
#include "fuzz.hpp"

namespace formats = liquid::formats;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 2);
    if (!input) {
        return 0;
    }
    const auto text = liquid::fuzz::text(input->payload);
    if (input->decoder == 0) {
        const auto parsed = formats::json::parse(text);
        if (!parsed) {
            return 0;
        }
        const auto written = formats::json::write(*parsed);
        if (written) {
            const auto again = formats::json::parse(*written);
            liquid::fuzz::require(again && *again == *parsed);
        }
    } else {
        const auto parsed = formats::dotenv::parse(text);
        if (!parsed) {
            return 0;
        }
        // Not a byte-for-byte property: parse drops one trailing '\r' per line, so a value that
        // ends in "\r\r" comes back one '\r' shorter. The written file must parse, with the same
        // keys in the same order.
        const auto written = formats::dotenv::write(*parsed);
        if (written) {
            const auto again = formats::dotenv::parse(*written);
            liquid::fuzz::require(again.has_value());
            liquid::fuzz::require(again->lines.size() == parsed->lines.size());
            for (std::size_t index = 0; index < parsed->lines.size(); ++index) {
                liquid::fuzz::require(again->lines[index].key == parsed->lines[index].key);
            }
        }
    }
    return 0;
}
