// SPDX-License-Identifier: MIT

// The container readers under src/formats/ (liquid #463): ZIP and the ZIP-based documents and
// packages (OOXML, ODF, wheel, JAR), tar, the Debian, RPM and Alpine package readers, and the ar
// members Debian decoding reads. Each codec goes through codec.hpp's driver.
//
//   0-8  zip, ooxml, odf, wheel, jar, tar, deb, rpm, apk
//   9    ar members (formats::ar::read)
//   10   the whole registry: recognise by magic and unpack, descending into nested containers

#include <cstddef>
#include <cstdint>

#include "codec.hpp"
#include "formats/ar/ar.hpp"
#include "fuzz.hpp"

namespace detail = liquid::formats::detail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const auto codecs = liquid::fuzz::link_codecs({detail::zip_module(),
                                                          detail::ooxml_module(),
                                                          detail::odf_module(),
                                                          detail::wheel_module(),
                                                          detail::jar_module(),
                                                          detail::tar_module(),
                                                          detail::deb_module(),
                                                          detail::rpm_module(),
                                                          detail::apk_module()});
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), codecs.size() + 2);
    if (!input) {
        return 0;
    }
    if (input->decoder < codecs.size()) {
        liquid::fuzz::exercise(codecs[input->decoder], input->payload);
    } else if (input->decoder == codecs.size()) {
        (void)liquid::formats::ar::read(input->payload);
    } else {
        liquid::fuzz::exercise_registry(input->payload);
    }
    return 0;
}
