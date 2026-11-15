// The store's own files as an open store reads them back from its backend (liquid #463): pack
// files (the frame scan of an open pack, the table of a sealed one, and each frame read back and
// decompressed through read_pack_entry) and the chunk index (its MANIFEST and the runs it names).
//
// The input goes into an in-memory backend, not onto disk. The footer, manifest and run header
// carry an FNV-1a checksum; the decoders that "repair" recompute it, so the fuzzer reaches the
// structure behind the checksum instead of stopping at it.
//
//   0  a pack file, as it is
//   1  a pack file with its footer's table checksum recomputed
//   2  an index: a length-prefixed MANIFEST (checksum recomputed), length-prefixed 32-byte ids to
//      look up, and the rest of the input as the run the manifest's first record names (its
//      header checksum recomputed). A run header is 262,188 bytes, so the seed sets max_len.
//   3  an index the harness lays out around fuzzed records: length-prefixed 48-byte records, a
//      bloom filter and ids to look up; the run header, fanout and manifest are made consistent.

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "fuzz.hpp"
#include "memory_backend.hpp"
#include "store/index.hpp"
#include "store/pack.hpp"

namespace store = liquid::store_detail;

namespace {

constexpr std::size_t pack_header_size = 16;
constexpr std::size_t footer_size = 32;
constexpr std::size_t fanout_count = 65'537;
constexpr std::size_t run_header_size = 8 + 1 + 7 + 8 + 8 + fanout_count * 4 + 8;
constexpr std::size_t record_size = 48;
constexpr std::size_t entry_limit = 64;
constexpr std::size_t record_limit = 1024;

constexpr std::array<std::byte, 8> run_magic{
    std::byte{'L'},
    std::byte{'Q'},
    std::byte{'I'},
    std::byte{'D'},
    std::byte{'X'},
    std::byte{1},
    std::byte{0},
    std::byte{0},
};
constexpr std::array<std::byte, 8> manifest_magic{
    std::byte{'L'},
    std::byte{'Q'},
    std::byte{'M'},
    std::byte{'A'},
    std::byte{'N'},
    std::byte{2},
    std::byte{0},
    std::byte{0},
};

using bytes = std::vector<std::byte>;

auto checksum(std::span<const std::byte> input) -> std::uint64_t {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : input) {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

auto get_u32(std::span<const std::byte> input, std::size_t at) -> std::uint32_t {
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        value |= std::uint32_t(std::to_integer<unsigned char>(input[at + i])) << (i * 8U);
    }
    return value;
}

auto get_u64(std::span<const std::byte> input, std::size_t at) -> std::uint64_t {
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) {
        value |= std::uint64_t(std::to_integer<unsigned char>(input[at + i])) << (i * 8U);
    }
    return value;
}

void set_u64(bytes& output, std::size_t at, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) {
        output[at + i] = std::byte{static_cast<unsigned char>(value >> (i * 8U))};
    }
}

void put_u32(bytes& output, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) {
        output.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
    }
}

void put_u64(bytes& output, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) {
        output.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
    }
}

// Takes one two-byte length-prefixed part from the front of the input (fuzz.hpp's layout) and
// leaves the rest.
auto take_part(std::span<const std::byte>& input) -> std::span<const std::byte> {
    if (input.size() < 2) {
        input = {};
        return {};
    }
    const auto length =
        (std::to_integer<std::size_t>(input[0]) << 8U) | std::to_integer<std::size_t>(input[1]);
    input = input.subspan(2);
    const auto taken = length < input.size() ? length : input.size();
    const auto part = input.first(taken);
    input = input.subspan(taken);
    return part;
}

void read_pack(std::span<const std::byte> payload, bool repair) {
    auto file = bytes(payload.begin(), payload.end());
    if (repair && file.size() >= pack_header_size + footer_size) {
        const auto footer = file.size() - footer_size;
        const auto table_at = get_u64(file, footer + 8);
        const auto table_size = get_u64(file, footer + 16);
        if (table_at <= footer && table_size == footer - table_at) {
            set_u64(file,
                    footer + 24,
                    checksum(std::span<const std::byte>{file}.subspan(
                        static_cast<std::size_t>(table_at), static_cast<std::size_t>(table_size))));
        }
    }
    const auto id = file.size() >= 12 ? get_u32(file, 8) : 0U;
    auto backend = std::make_shared<liquid::fuzz::memory_backend>();
    if (!liquid::runtime::block_on(backend->put("pack", file))) {
        return;
    }
    const auto contents = liquid::runtime::block_on(store::read_pack(backend, "pack", id));
    if (!contents) {
        return;
    }
    auto key = liquid::crypto_detail::key{};
    key.fill(std::byte{0x2a});
    auto read = std::size_t{};
    for (const auto& entry : contents->entries) {
        if (read++ == entry_limit) {
            break;
        }
        auto dictionary = std::optional<store::delta_dictionary>{};
        if (entry.base) {
            dictionary = store::delta_dictionary{*entry.base, payload};
        }
        (void)liquid::runtime::block_on(
            store::read_pack_entry(backend, "pack", entry.location, entry.id, key, dictionary));
    }
}

void find_all(const std::shared_ptr<liquid::fuzz::memory_backend>& backend,
              std::span<const std::byte> ids) {
    auto index = store::chunk_index{backend, "index", "fuzz"};
    if (!liquid::runtime::block_on(index.open())) {
        return;
    }
    auto looked = std::size_t{};
    for (std::size_t at = 0; at + 32 <= ids.size() && looked < entry_limit; at += 32, ++looked) {
        for (const auto alg : {liquid::algorithm::blake3,
                               liquid::algorithm::sha256,
                               liquid::algorithm::blake3_keyed}) {
            auto id = liquid::digest{alg, {}};
            std::memcpy(id.bytes.data(), ids.data() + at, id.bytes.size());
            (void)index.find(id);
        }
    }
}

void raw_index(std::span<const std::byte> payload) {
    auto rest = payload;
    const auto manifest_part = take_part(rest);
    const auto ids = take_part(rest);
    auto manifest = bytes(manifest_part.begin(), manifest_part.end());
    if (manifest.size() >= 8) {
        set_u64(manifest,
                manifest.size() - 8,
                checksum(std::span<const std::byte>{manifest}.first(manifest.size() - 8)));
    }
    auto run = bytes(rest.begin(), rest.end());
    if (run.size() >= run_header_size) {
        set_u64(run,
                run_header_size - 8,
                checksum(std::span<const std::byte>{run}.first(run_header_size - 8)));
    }
    const auto version_two = manifest.size() > 5 && manifest[5] == std::byte{2};
    const auto records_offset = version_two ? std::size_t{24} : std::size_t{16};
    const auto sequence =
        manifest.size() >= records_offset + 16 ? get_u64(manifest, records_offset + 8) : 1U;
    auto backend = std::make_shared<liquid::fuzz::memory_backend>();
    (void)liquid::runtime::block_on(backend->put("index/MANIFEST", std::move(manifest)));
    (void)liquid::runtime::block_on(
        backend->put("index/run-" + std::to_string(sequence) + ".idx", std::move(run)));
    find_all(backend, ids);
}

void structured_index(std::span<const std::byte> payload) {
    const auto split = liquid::fuzz::parts(payload, 3);
    if (split.empty()) {
        return;
    }
    const auto count = std::min(split[0].size() / record_size, record_limit);
    const auto records = split[0].first(count * record_size);
    auto bloom = split.size() > 1 ? bytes(split[1].begin(), split[1].end()) : bytes{};
    if (bloom.empty()) {
        // An empty bloom filter answers "absent" for every id, so the lookup never reads a run.
        bloom.assign(8, std::byte{0xff});
    }
    auto fanout = std::vector<std::uint32_t>(fanout_count);
    for (std::size_t at = 0; at < records.size(); at += record_size) {
        const auto bucket = (std::to_integer<std::size_t>(records[at]) << 8U) |
                            std::to_integer<std::size_t>(records[at + 1]);
        ++fanout[bucket + 1];
    }
    for (std::size_t i = 1; i < fanout.size(); ++i) {
        fanout[i] += fanout[i - 1];
    }
    auto run = bytes(run_magic.begin(), run_magic.end());
    run.push_back(std::byte{static_cast<unsigned char>(liquid::algorithm::blake3)});
    run.insert(run.end(), 7, std::byte{0});
    put_u64(run, count);
    put_u64(run, bloom.size());
    const auto fanout_at = run.size();
    run.resize(fanout_at + fanout.size() * sizeof(std::uint32_t));
    // The reader maps the fanout as native std::uint32_t values.
    std::memcpy(run.data() + fanout_at, fanout.data(), fanout.size() * sizeof(std::uint32_t));
    put_u64(run, checksum(run));
    run.insert(run.end(), bloom.begin(), bloom.end());
    run.insert(run.end(), records.begin(), records.end());

    auto manifest = bytes(manifest_magic.begin(), manifest_magic.end());
    put_u32(manifest, 1);
    put_u32(manifest, 0);
    put_u64(manifest, 0);
    manifest.push_back(std::byte{static_cast<unsigned char>(liquid::algorithm::blake3)});
    manifest.insert(manifest.end(), 7, std::byte{0});
    put_u64(manifest, 1);
    put_u64(manifest, count);
    put_u64(manifest, checksum(manifest));

    auto backend = std::make_shared<liquid::fuzz::memory_backend>();
    (void)liquid::runtime::block_on(backend->put("index/MANIFEST", std::move(manifest)));
    (void)liquid::runtime::block_on(backend->put("index/run-1.idx", std::move(run)));
    auto ids = bytes{};
    for (std::size_t at = 0; at < records.size(); at += record_size) {
        ids.insert(ids.end(), records.begin() + at, records.begin() + at + 32);
    }
    if (split.size() > 2) {
        ids.insert(ids.end(), split[2].begin(), split[2].end());
    }
    find_all(backend, ids);
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const std::uint8_t* data, std::size_t size) {
    const auto input = liquid::fuzz::select(liquid::fuzz::bytes(data, size), 4);
    if (!input) {
        return 0;
    }
    switch (input->decoder) {
    case 0:
        read_pack(input->payload, false);
        break;
    case 1:
        read_pack(input->payload, true);
        break;
    case 2:
        raw_index(input->payload);
        break;
    default:
        structured_index(input->payload);
        break;
    }
    return 0;
}
