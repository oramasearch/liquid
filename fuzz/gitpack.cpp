// Git pack files and pack indexes as `liquid import` and the Git bridge read them from disk through
// git_detail::object_store (liquid #463): the version 2 index (fanout, names, 31-bit and 64-bit
// offsets), the packed object headers, OFS_DELTA and REF_DELTA bases, zlib inflation and delta
// application. The loose-object and delta parsers on their own are the `git` harness.
//
// object_store reads only from a Git directory, so the harness writes the input to
// <temporary directory>/liquid-fuzz-git-pack-<pid>/objects/pack/ and reads from there.
//
//   0  two length-prefixed parts: the .idx file and the .pack file, as they are
//   1  two length-prefixed parts: 24-byte index records (a 20-byte object id and a big-endian
//      31-bit offset) from which the harness writes a well-formed version 2 index, and the .pack
//      file
//
// Every object id the index lists (at most 64) is read, then the commit and tree parsers run on
// whatever was read.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include <unistd.h>

#include "fuzz.hpp"
#include "git/object_store.hpp"
#include "git/sha1.hpp"

namespace fs = std::filesystem;
namespace git = liquid::git_detail;

namespace {

constexpr std::size_t read_limit = 64;
constexpr std::size_t names_offset = 8 + 256 * 4;

auto directory() -> const fs::path& {
    static const auto path = [] {
        auto output =
            fs::temp_directory_path() / ("liquid-fuzz-git-pack-" + std::to_string(::getpid()));
        std::error_code ignored;
        fs::remove_all(output, ignored);
        fs::create_directories(output / "objects" / "pack");
        return output;
    }();
    return path;
}

void write(const fs::path& path, std::span<const std::byte> input) {
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(input.data()),
                 static_cast<std::streamsize>(input.size()));
}

void put_u32(std::vector<std::byte>& output, std::uint32_t value) {
    for (int shift = 24; shift >= 0; shift -= 8) {
        output.push_back(std::byte{static_cast<unsigned char>(value >> shift)});
    }
}

auto get_u32(std::span<const std::byte> input, std::size_t at) -> std::uint32_t {
    return (std::to_integer<std::uint32_t>(input[at]) << 24U) |
           (std::to_integer<std::uint32_t>(input[at + 1]) << 16U) |
           (std::to_integer<std::uint32_t>(input[at + 2]) << 8U) |
           std::to_integer<std::uint32_t>(input[at + 3]);
}

struct record {
    git::sha1 id{};
    std::uint32_t offset{};
};

// A version 2 index for the given records: magic, version, fanout, sorted names, CRCs (zero; the
// reader does not check them) and 31-bit offsets.
auto make_index(std::span<const std::byte> input) -> std::vector<std::byte> {
    auto records = std::vector<record>{};
    for (std::size_t at = 0; at + 24 <= input.size() && records.size() < read_limit; at += 24) {
        auto value = record{};
        std::copy_n(input.begin() + static_cast<std::ptrdiff_t>(at), 20, value.id.begin());
        value.offset = get_u32(input, at + 20) & 0x7fffffffU;
        records.push_back(value);
    }
    std::ranges::sort(records, {}, &record::id);
    auto output = std::vector<std::byte>{};
    put_u32(output, 0xff744f63U);
    put_u32(output, 2);
    auto fanout = std::array<std::uint32_t, 256>{};
    for (const auto& value : records) {
        ++fanout[std::to_integer<std::size_t>(value.id.front())];
    }
    auto running = std::uint32_t{};
    for (const auto count : fanout) {
        running += count;
        put_u32(output, running);
    }
    for (const auto& value : records) {
        output.insert(output.end(), value.id.begin(), value.id.end());
    }
    for (std::size_t i = 0; i < records.size(); ++i) {
        put_u32(output, 0);
    }
    for (const auto& value : records) {
        put_u32(output, value.offset);
    }
    return output;
}

// The ids an index lists, read the way object_store reads them, bounded by the file's size.
auto listed_ids(std::span<const std::byte> index) -> std::vector<git::sha1> {
    auto output = std::vector<git::sha1>{};
    if (index.size() < names_offset) {
        return output;
    }
    const auto count = get_u32(index, names_offset - 4);
    for (std::uint32_t number = 0; number < count && output.size() < read_limit; ++number) {
        const auto at = names_offset + std::size_t{number} * 20;
        if (at + 20 > index.size()) {
            break;
        }
        auto id = git::sha1{};
        std::copy_n(index.begin() + static_cast<std::ptrdiff_t>(at), id.size(), id.begin());
        output.push_back(id);
    }
    return output;
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 2);
    if (!input) {
        return 0;
    }
    const auto split = liquid::fuzz::parts(input->payload, 2);
    if (split.size() != 2) {
        return 0;
    }
    const auto index = input->decoder == 0
                           ? std::vector<std::byte>(split[0].begin(), split[0].end())
                           : make_index(split[0]);
    const auto& root = directory();
    write(root / "objects" / "pack" / "pack-fuzz.idx", index);
    write(root / "objects" / "pack" / "pack-fuzz.pack", split[1]);
    auto objects = git::object_store{root};
    for (const auto& id : listed_ids(index)) {
        const auto object = objects.read(id);
        if (!object) {
            continue;
        }
        if (object->kind == git::object_kind::commit) {
            (void)git::parse_commit(*object);
        } else if (object->kind == git::object_kind::tree) {
            (void)git::parse_tree(*object);
        }
    }
    return 0;
}
