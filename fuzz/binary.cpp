// SPDX-License-Identifier: MIT

// The linked-binary readers under src/formats/binary/ (liquid #463): ELF, Mach-O and PE, both as
// registry codecs (codec.hpp's driver) and through the structured readers that SBOM inference,
// ABI extraction and relocation checking call directly.
//
//   0-2  the elf, macho and pe codecs
//   3-5  inspect_elf, inspect_macho, inspect_pe

#include <cstddef>
#include <cstdint>

#include "codec.hpp"
#include "formats/binary/elf.hpp"
#include "formats/binary/macho.hpp"
#include "formats/binary/pe.hpp"
#include "fuzz.hpp"

namespace binary = liquid::formats::binary;
namespace detail = liquid::formats::detail;

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    static const auto codecs = liquid::fuzz::link_codecs(
        {detail::elf_module(), detail::macho_module(), detail::pe_module()});
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 6);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0:
    case 1:
    case 2:
        if (input->decoder < codecs.size()) {
            liquid::fuzz::exercise(codecs[input->decoder], input->payload);
        }
        break;
    case 3:
        (void)binary::inspect_elf(input->payload);
        break;
    case 4:
        (void)binary::inspect_macho(input->payload);
        break;
    default:
        (void)binary::inspect_pe(input->payload);
        break;
    }
    return 0;
}
