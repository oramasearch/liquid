#include "index.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <map>
#include <string>

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> run_magic{std::byte{'L'}, std::byte{'Q'}, std::byte{'I'},
                                              std::byte{'D'}, std::byte{'X'}, std::byte{1},
                                              std::byte{0}, std::byte{0}};
constexpr std::array<std::byte, 8> manifest_magic{std::byte{'L'}, std::byte{'Q'}, std::byte{'M'},
                                                   std::byte{'A'}, std::byte{'N'}, std::byte{1},
                                                   std::byte{0}, std::byte{0}};
constexpr std::size_t fanout_count = 257;
constexpr std::size_t run_header_size = 8 + 1 + 7 + 8 + 8 + fanout_count * 8 + 8;

error index_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}
void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
}
void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
}
std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 4 > bytes.size()) return 0;
    std::uint32_t out = 0;
    for (unsigned i = 0; i != 4; ++i) out |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + i])) << (i * 8U);
    return out;
}
std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 8 > bytes.size()) return 0;
    std::uint64_t out = 0;
    for (unsigned i = 0; i != 8; ++i) out |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + i])) << (i * 8U);
    return out;
}
std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t out = 1469598103934665603ULL;
    for (const auto byte : bytes) { out ^= std::to_integer<unsigned char>(byte); out *= 1099511628211ULL; }
    return out;
}
bool valid_algorithm(unsigned char value) {
    return value == static_cast<unsigned char>(algorithm::blake3) || value == static_cast<unsigned char>(algorithm::sha256);
}
bool before(const index_entry& left, const index_entry& right) { return left.id.bytes < right.id.bytes; }
bool equal_id(const index_entry& left, const digest& right) { return left.id.bytes == right.bytes; }

std::vector<std::byte> encode_records(const std::vector<index_entry>& entries) {
    std::vector<std::byte> bytes;
    bytes.reserve(entries.size() * 48);
    for (const auto& entry : entries) {
        bytes.insert(bytes.end(), entry.id.bytes.begin(), entry.id.bytes.end());
        put_u32(bytes, entry.location.pack_id);
        put_u64(bytes, entry.location.offset);
        put_u32(bytes, entry.location.length);
    }
    return bytes;
}

}  // namespace

chunk_index::chunk_index(std::filesystem::path root) : root_(std::move(root)) {}

runtime::task<void> chunk_index::open() {
    try {
        std::filesystem::create_directories(root_);
        const auto manifest = root_ / "MANIFEST";
        if (!std::filesystem::exists(manifest)) co_return result<void>{};
        auto raw = co_await read_file(manifest);
        if (!raw) co_return std::unexpected(raw.error());
        const auto bytes = std::span<const std::byte>(*raw);
        if (bytes.size() < 8 + 4 + 4 + 8 || !std::equal(manifest_magic.begin(), manifest_magic.end(), bytes.begin())) {
            co_return std::unexpected(index_error(errc::corrupt, "invalid index manifest", manifest));
        }
        const auto count = get_u32(bytes, 8);
        highest_pack_id_ = get_u32(bytes, 12);
        if (bytes.size() != 16 + std::size_t(count) * 24 + 8 || checksum(bytes.first(bytes.size() - 8)) != get_u64(bytes, bytes.size() - 8)) {
            co_return std::unexpected(index_error(errc::corrupt, "corrupt index manifest", manifest));
        }
        for (std::size_t n = 0, at = 16; n < count; ++n, at += 24) {
            const auto alg_byte = std::to_integer<unsigned char>(bytes[at]);
            if (!valid_algorithm(alg_byte)) co_return std::unexpected(index_error(errc::corrupt, "unknown run algorithm", manifest));
            const auto seq = get_u64(bytes, at + 8);
            const auto expected_count = get_u64(bytes, at + 16);
            const auto path = root_ / ("run-" + std::to_string(seq) + ".idx");
            auto run_bytes = co_await read_file(path);
            if (!run_bytes) co_return std::unexpected(run_bytes.error());
            const auto input = std::span<const std::byte>(*run_bytes);
            if (input.size() < run_header_size || !std::equal(run_magic.begin(), run_magic.end(), input.begin()) ||
                std::to_integer<unsigned char>(input[8]) != alg_byte || get_u64(input, 16) != expected_count ||
                checksum(input.first(run_header_size - 8)) != get_u64(input, run_header_size - 8)) {
                co_return std::unexpected(index_error(errc::corrupt, "corrupt index run header", path));
            }
            const auto bloom_size = get_u64(input, 24);
            if (input.size() != run_header_size + bloom_size + expected_count * 48) {
                co_return std::unexpected(index_error(errc::corrupt, "invalid index run size", path));
            }
            run loaded{seq, static_cast<algorithm>(alg_byte), path, {}, {}};
            loaded.bloom.assign(input.begin() + run_header_size, input.begin() + run_header_size + bloom_size);
            const auto records = input.subspan(run_header_size + bloom_size);
            loaded.entries.reserve(expected_count);
            for (std::size_t at_record = 0; at_record < records.size(); at_record += 48) {
                index_entry entry{};
                entry.id.alg = loaded.algorithm_value;
                std::copy_n(records.begin() + at_record, 32, entry.id.bytes.begin());
                entry.location = {get_u32(records, at_record + 32), get_u64(records, at_record + 36), get_u32(records, at_record + 44)};
                loaded.entries.push_back(entry);
            }
            states_[loaded.algorithm_value].runs.push_back(std::move(loaded));
            next_sequence_ = std::max(next_sequence_, seq + 1);
        }
        for (auto& [unused, state] : states_) {
            std::sort(state.runs.begin(), state.runs.end(), [](const run& a, const run& b) { return a.sequence > b.sequence; });
        }
        co_return result<void>{};
    } catch (const std::exception& exception) {
        co_return std::unexpected(index_error(errc::io, exception.what(), root_));
    }
}

runtime::task<void> chunk_index::insert(index_entry entry) {
    auto existing = co_await find(entry.id);
    if (!existing) co_return std::unexpected(existing.error());
    if (!*existing) states_[entry.id.alg].memtable.push_back(std::move(entry));
    co_return result<void>{};
}

runtime::task<void> chunk_index::flush() {
    for (auto& [alg, state] : states_) {
        if (state.memtable.empty()) continue;
        auto entries = std::move(state.memtable);
        state.memtable.clear();
        std::sort(entries.begin(), entries.end(), before);
        entries.erase(std::unique(entries.begin(), entries.end(), [](const index_entry& a, const index_entry& b) {
            return a.id.bytes == b.id.bytes;
        }), entries.end());
        std::array<std::uint64_t, fanout_count> fanout{};
        for (const auto& entry : entries) ++fanout[std::to_integer<unsigned char>(entry.id.bytes[0]) + 1];
        for (std::size_t i = 1; i < fanout.size(); ++i) fanout[i] += fanout[i - 1];
        const auto bloom_bits = std::max<std::size_t>(8, entries.size() * 10);
        std::vector<std::byte> bloom((bloom_bits + 7) / 8);
        const auto set_bloom = [&](const digest& id) {
            std::uint64_t h = 1469598103934665603ULL;
            for (auto b : id.bytes) { h ^= std::to_integer<unsigned char>(b); h *= 1099511628211ULL; }
            for (unsigned n = 0; n < 7; ++n) {
                const auto bit = (h + n * 0x9e3779b97f4a7c15ULL) % bloom_bits;
                bloom[bit / 8] |= std::byte{static_cast<unsigned char>(1U << (bit % 8))};
            }
        };
        for (const auto& entry : entries) set_bloom(entry.id);
        std::vector<std::byte> header;
        header.reserve(run_header_size);
        header.insert(header.end(), run_magic.begin(), run_magic.end());
        header.push_back(std::byte{static_cast<unsigned char>(alg)});
        header.insert(header.end(), 7, std::byte{0});
        put_u64(header, entries.size());
        put_u64(header, bloom.size());
        for (auto value : fanout) put_u64(header, value);
        put_u64(header, checksum(header));
        auto records = encode_records(entries);
        header.insert(header.end(), bloom.begin(), bloom.end());
        header.insert(header.end(), records.begin(), records.end());
        const auto sequence = next_sequence_++;
        const auto path = root_ / ("run-" + std::to_string(sequence) + ".idx");
        auto saved = co_await write_file(path, std::move(header));
        if (!saved) co_return std::unexpected(saved.error());
        state.runs.insert(state.runs.begin(), run{sequence, alg, path, std::move(entries), std::move(bloom)});
        if (state.runs.size() > 4) {
            auto compacted = co_await compact(alg);
            if (!compacted) co_return std::unexpected(compacted.error());
        }
    }
    co_return co_await write_manifest();
}

runtime::task<std::optional<chunk_location>> chunk_index::find(digest id) {
    const auto found = [&](const std::vector<index_entry>& entries) -> std::optional<chunk_location> {
        const auto it = std::lower_bound(entries.begin(), entries.end(), id, [](const index_entry& entry, const digest& value) {
            return entry.id.bytes < value.bytes;
        });
        if (it != entries.end() && equal_id(*it, id)) return it->location;
        return std::nullopt;
    };
    const auto it = states_.find(id.alg);
    if (it == states_.end()) co_return std::nullopt;
    auto mem = it->second.memtable;
    std::sort(mem.begin(), mem.end(), before);
    if (auto result = found(mem)) co_return result;
    for (const auto& run : it->second.runs) {
        if (auto result = found(run.entries)) co_return result;
    }
    co_return std::nullopt;
}

runtime::task<void> chunk_index::compact(algorithm algorithm_value) {
    auto& state = states_[algorithm_value];
    std::map<std::array<std::byte, 32>, index_entry> merged;
    for (auto it = state.runs.rbegin(); it != state.runs.rend(); ++it) {
        for (const auto& entry : it->entries) merged[entry.id.bytes] = entry;
    }
    std::vector<index_entry> entries;
    entries.reserve(merged.size());
    for (auto& [unused, entry] : merged) entries.push_back(std::move(entry));
    const auto old_runs = std::move(state.runs);
    state.runs.clear();
    state.memtable = std::move(entries);
    auto output = co_await flush();
    if (!output) co_return std::unexpected(output.error());
    for (const auto& old : old_runs) std::filesystem::remove(old.path);
    co_return result<void>{};
}

runtime::task<void> chunk_index::write_manifest() {
    std::vector<std::byte> bytes;
    bytes.insert(bytes.end(), manifest_magic.begin(), manifest_magic.end());
    std::uint32_t count = 0;
    for (const auto& [unused, state] : states_) count += static_cast<std::uint32_t>(state.runs.size());
    put_u32(bytes, count);
    put_u32(bytes, highest_pack_id_);
    for (const auto& [alg, state] : states_) for (const auto& run : state.runs) {
        bytes.push_back(std::byte{static_cast<unsigned char>(alg)});
        bytes.insert(bytes.end(), 7, std::byte{0});
        put_u64(bytes, run.sequence);
        put_u64(bytes, run.entries.size());
    }
    put_u64(bytes, checksum(bytes));
    const auto temporary = root_ / "MANIFEST.tmp";
    auto saved = co_await write_file(temporary, std::move(bytes));
    if (!saved) co_return std::unexpected(saved.error());
    try {
        std::filesystem::rename(temporary, root_ / "MANIFEST");
    } catch (const std::exception& exception) {
        co_return std::unexpected(index_error(errc::io, exception.what(), root_ / "MANIFEST"));
    }
    co_return result<void>{};
}

runtime::task<void> chunk_index::rebuild(std::vector<index_entry> entries) {
    try {
        std::filesystem::remove_all(root_);
        std::filesystem::create_directories(root_);
        states_.clear();
        next_sequence_ = 1;
        for (auto& entry : entries) states_[entry.id.alg].memtable.push_back(std::move(entry));
        co_return co_await flush();
    } catch (const std::exception& exception) {
        co_return std::unexpected(index_error(errc::io, exception.what(), root_));
    }
}

runtime::task<void> chunk_index::close() { co_return co_await flush(); }

}  // namespace liquid::store_detail
