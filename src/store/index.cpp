#include "index.hpp"

#include "backend.hpp"
#include "faults.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>
#include <span>
#include <string>
#include <utility>

namespace liquid::store_detail {
namespace {

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
constexpr std::array<std::byte, 8> manifest_magic_v1{
    std::byte{'L'},
    std::byte{'Q'},
    std::byte{'M'},
    std::byte{'A'},
    std::byte{'N'},
    std::byte{1},
    std::byte{0},
    std::byte{0},
};
constexpr std::array<std::byte, 8> manifest_magic_v2{
    std::byte{'L'},
    std::byte{'Q'},
    std::byte{'M'},
    std::byte{'A'},
    std::byte{'N'},
    std::byte{2},
    std::byte{0},
    std::byte{0},
};
constexpr std::size_t record_size = 48;
constexpr std::size_t fanout_count = 65'537;
constexpr std::size_t run_header_size =
    8 + 1 + 7 + 8 + 8 + fanout_count * sizeof(std::uint32_t) + 8;
constexpr std::size_t write_window_size = 4 * 1024 * 1024;

error index_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned i = 0; i != 4; ++i) {
        out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
    }
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned i = 0; i != 8; ++i) {
        out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
    }
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 4 > bytes.size()) {
        return 0;
    }
    std::uint32_t value = 0;
    for (unsigned i = 0; i != 4; ++i) {
        value |= std::uint32_t(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8U);
    }
    return value;
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t offset) {
    if (offset + 8 > bytes.size()) {
        return 0;
    }
    std::uint64_t value = 0;
    for (unsigned i = 0; i != 8; ++i) {
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[offset + i])) << (i * 8U);
    }
    return value;
}

std::uint64_t checksum(std::span<const std::byte> bytes) {
    std::uint64_t value = 1469598103934665603ULL;
    for (const auto byte : bytes) {
        value ^= std::to_integer<unsigned char>(byte);
        value *= 1099511628211ULL;
    }
    return value;
}

bool valid_algorithm(unsigned char value) {
    return value == static_cast<unsigned char>(algorithm::blake3) ||
           value == static_cast<unsigned char>(algorithm::sha256) ||
           value == static_cast<unsigned char>(algorithm::blake3_keyed);
}

std::uint64_t bloom_hash(const std::array<std::byte, 32>& id) {
    return checksum(id);
}

void set_bloom(std::vector<std::byte>& bloom, const std::array<std::byte, 32>& id) {
    const auto bit_count = bloom.size() * 8;
    const auto hash = bloom_hash(id);
    for (unsigned i = 0; i != 7; ++i) {
        const auto bit = (hash + i * 0x9e3779b97f4a7c15ULL) % bit_count;
        bloom[bit / 8] |= std::byte{static_cast<unsigned char>(1U << (bit % 8))};
    }
}

bool bloom_may_contain(std::span<const std::byte> bloom, const std::array<std::byte, 32>& id) {
    if (bloom.empty()) {
        return false;
    }
    const auto bit_count = bloom.size() * 8;
    const auto hash = bloom_hash(id);
    for (unsigned i = 0; i != 7; ++i) {
        const auto bit = (hash + i * 0x9e3779b97f4a7c15ULL) % bit_count;
        if ((std::to_integer<unsigned char>(bloom[bit / 8]) & (1U << (bit % 8))) == 0) {
            return false;
        }
    }
    return true;
}

index_entry decode_record(std::span<const std::byte> bytes, algorithm alg) {
    index_entry entry{};
    entry.id.alg = alg;
    std::copy_n(bytes.begin(), 32, entry.id.bytes.begin());
    entry.location = {get_u32(bytes, 32), get_u64(bytes, 36), get_u32(bytes, 44)};
    return entry;
}

void append_record(std::vector<std::byte>& bytes,
                   const std::array<std::byte, 32>& id,
                   chunk_location location) {
    bytes.insert(bytes.end(), id.begin(), id.end());
    put_u32(bytes, location.pack_id);
    put_u64(bytes, location.offset);
    put_u32(bytes, location.length);
}

std::vector<std::byte> make_header(algorithm alg,
                                   std::uint64_t count,
                                   std::uint64_t bloom_size,
                                   std::span<const std::uint32_t> fanout) {
    std::vector<std::byte> header;
    header.reserve(run_header_size);
    header.insert(header.end(), run_magic.begin(), run_magic.end());
    header.push_back(std::byte{static_cast<unsigned char>(alg)});
    header.insert(header.end(), 7, std::byte{0});
    put_u64(header, count);
    put_u64(header, bloom_size);
    for (const auto value : fanout) {
        put_u32(header, value);
    }
    put_u64(header, checksum(header));
    return header;
}

unsigned tier(std::uint64_t records) {
    unsigned value = 0;
    while (records > 1) {
        records >>= 1;
        ++value;
    }
    return value;
}

std::uint32_t fanout_bucket(const std::array<std::byte, 32>& id) {
    return (std::uint32_t(std::to_integer<unsigned char>(id[0])) << 8U) |
           std::to_integer<unsigned char>(id[1]);
}

// One input of a compaction, copied by value: the merge suspends on every write, and a concurrent
// flush reallocates `algorithm_state::runs` (issue #271).
struct merge_input {
    std::uint64_t sequence{};
    std::uint64_t record_count{};
    std::uint64_t records_offset{};
    std::span<const std::byte> bytes;
};

// A k-way merge over sorted input runs. It yields each digest once, with the location from the
// newest run holding it. It only reads the inputs, so a second merge over the same inputs yields
// the same records: compaction runs one to size the run and one to write it (liquid #332).
class run_merge {
public:
    run_merge(std::span<const merge_input> input, algorithm alg) : input_(input), alg_(alg) {
        positions_.assign(input_.size(), 0);
        heads_.reserve(input_.size());
        for (const auto& source : input_) {
            heads_.push_back(
                decode_record(source.bytes.subspan(source.records_offset, record_size), alg_));
        }
    }

    std::optional<index_entry> next() {
        std::optional<std::size_t> choice;
        for (std::size_t i = 0; i < heads_.size(); ++i) {
            if (heads_[i] && (!choice || heads_[i]->id.bytes < heads_[*choice]->id.bytes)) {
                choice = i;
            }
        }
        if (!choice) {
            return std::nullopt;
        }
        const auto key = heads_[*choice]->id.bytes;
        std::size_t newest = *choice;
        for (std::size_t i = 0; i < heads_.size(); ++i) {
            if (heads_[i] && heads_[i]->id.bytes == key &&
                input_[i].sequence > input_[newest].sequence) {
                newest = i;
            }
        }
        const auto output = *heads_[newest];
        for (std::size_t i = 0; i < heads_.size(); ++i) {
            if (!heads_[i] || heads_[i]->id.bytes != key) {
                continue;
            }
            ++positions_[i];
            if (positions_[i] == input_[i].record_count) {
                heads_[i] = std::nullopt;
            } else {
                heads_[i] = decode_record(
                    input_[i].bytes.subspan(input_[i].records_offset + positions_[i] * record_size,
                                            record_size),
                    alg_);
            }
        }
        return output;
    }

private:
    std::span<const merge_input> input_;
    algorithm alg_;
    std::vector<std::uint64_t> positions_;
    std::vector<std::optional<index_entry>> heads_;
};

// Appends `bytes` at `offset`, the object's current end, in windows of at most write_window_size.
runtime::task<void> append_windows(std::shared_ptr<::liquid::store_backend> backend,
                                   std::string key,
                                   std::uint64_t offset,
                                   std::span<const std::byte> bytes) {
    while (!bytes.empty()) {
        const auto size = std::min(bytes.size(), write_window_size);
        auto written = co_await backend->put_range(
            key, offset, std::vector<std::byte>(bytes.begin(), bytes.begin() + size));
        if (!written) {
            co_return std::unexpected(written.error());
        }
        offset += size;
        bytes = bytes.subspan(size);
    }
    co_return result<void>{};
}

} // namespace

chunk_index::chunk_index(std::filesystem::path root, chunk_index_options options)
    : root_(std::move(root)), backend_(make_disk_store_backend(root_)), options_(options) {
    options_.memtable_entries = std::max<std::size_t>(1, options_.memtable_entries);
    options_.compaction_runs = std::max<std::size_t>(1, options_.compaction_runs);
}

chunk_index::chunk_index(std::shared_ptr<::liquid::store_backend> backend,
                         std::string prefix,
                         std::filesystem::path context,
                         chunk_index_options options)
    : root_(std::move(context)), backend_(std::move(backend)), prefix_(std::move(prefix)),
      options_(options) {
    options_.memtable_entries = std::max<std::size_t>(1, options_.memtable_entries);
    options_.compaction_runs = std::max<std::size_t>(1, options_.compaction_runs);
}

std::string chunk_index::key(std::string_view name) const {
    return prefix_.empty() ? std::string(name) : prefix_ + "/" + std::string(name);
}

std::size_t
chunk_index::digest_hash::operator()(const std::array<std::byte, 32>& value) const noexcept {
    return static_cast<std::size_t>(bloom_hash(value));
}

runtime::task<chunk_index::run>
chunk_index::map_run(std::uint64_t sequence, algorithm alg, std::uint64_t expected_count) const {
    run output{};
    output.sequence = sequence;
    output.algorithm_value = alg;
    output.key = key("run-" + std::to_string(sequence) + ".idx");
    auto mapped = co_await output.mapping.open(backend_, output.key);
    if (!mapped) {
        co_return std::unexpected(mapped.error());
    }
    const auto bytes = output.mapping.bytes();
    if (bytes.size() < run_header_size ||
        !std::equal(run_magic.begin(), run_magic.end(), bytes.begin()) ||
        std::to_integer<unsigned char>(bytes[8]) != static_cast<unsigned char>(alg) ||
        get_u64(bytes, 16) != expected_count ||
        checksum(bytes.first(run_header_size - 8)) != get_u64(bytes, run_header_size - 8)) {
        co_return std::unexpected(index_error(errc::corrupt, "corrupt index run header", root_));
    }
    const auto bloom_size = get_u64(bytes, 24);
    if (bloom_size > std::numeric_limits<std::size_t>::max() ||
        bytes.size() != run_header_size + bloom_size + expected_count * record_size) {
        co_return std::unexpected(index_error(errc::corrupt, "invalid index run size", root_));
    }
    output.record_count = expected_count;
    output.fanout = std::span<const std::uint32_t>(
        reinterpret_cast<const std::uint32_t*>(bytes.data() + 32), fanout_count);
    if (output.fanout.front() != 0 || output.fanout.back() != expected_count ||
        !std::is_sorted(output.fanout.begin(), output.fanout.end())) {
        co_return std::unexpected(index_error(errc::corrupt, "invalid index fanout", root_));
    }
    output.bloom = bytes.subspan(run_header_size, static_cast<std::size_t>(bloom_size));
    output.records_offset = run_header_size + bloom_size;
    co_return output;
}

runtime::task<void> chunk_index::open() {
    try {
        states_.clear();
        next_sequence_ = 1;
        highest_pack_id_ = 0;
        collection_epoch_ = 0;
        const auto manifest_key = key("MANIFEST");
        auto manifest_exists = co_await backend_exists(backend_, manifest_key);
        if (!manifest_exists) {
            co_return std::unexpected(manifest_exists.error());
        }
        if (!*manifest_exists) {
            co_return result<void>{};
        }
        auto raw = co_await read_backend_object(backend_, manifest_key);
        if (!raw) {
            co_return std::unexpected(raw.error());
        }
        const auto bytes = std::span<const std::byte>(*raw);
        const bool version_one =
            bytes.size() >= 24 &&
            std::equal(manifest_magic_v1.begin(), manifest_magic_v1.end(), bytes.begin());
        const bool version_two =
            bytes.size() >= 32 &&
            std::equal(manifest_magic_v2.begin(), manifest_magic_v2.end(), bytes.begin());
        if (!version_one && !version_two) {
            co_return std::unexpected(
                index_error(errc::corrupt, "invalid index manifest", root_ / "MANIFEST"));
        }
        const auto count = get_u32(bytes, 8);
        highest_pack_id_ = get_u32(bytes, 12);
        const auto records_offset = version_two ? 24U : 16U;
        if (version_two)
            collection_epoch_ = get_u64(bytes, 16);
        if (bytes.size() != records_offset + std::size_t(count) * 24 + 8 ||
            checksum(bytes.first(bytes.size() - 8)) != get_u64(bytes, bytes.size() - 8)) {
            co_return std::unexpected(
                index_error(errc::corrupt, "corrupt index manifest", root_ / "MANIFEST"));
        }
        for (std::size_t i = 0, offset = records_offset; i < count; ++i, offset += 24) {
            const auto algorithm_byte = std::to_integer<unsigned char>(bytes[offset]);
            if (!valid_algorithm(algorithm_byte)) {
                co_return std::unexpected(
                    index_error(errc::corrupt, "unknown run algorithm", root_ / "MANIFEST"));
            }
            const auto sequence = get_u64(bytes, offset + 8);
            auto mapped = co_await map_run(
                sequence, static_cast<algorithm>(algorithm_byte), get_u64(bytes, offset + 16));
            if (!mapped) {
                co_return std::unexpected(mapped.error());
            }
            states_[mapped->algorithm_value].runs.push_back(std::move(*mapped));
            next_sequence_ = std::max(next_sequence_, sequence + 1);
        }
        for (auto& [ignored, state] : states_) {
            std::sort(state.runs.begin(), state.runs.end(), [](const run& left, const run& right) {
                return left.sequence > right.sequence;
            });
        }
        co_return result<void>{};
    } catch (const std::exception& exception) {
        co_return std::unexpected(index_error(errc::io, exception.what(), root_));
    }
}

result<void> chunk_index::insert(index_entry entry) {
    auto& state = states_[entry.id.alg];
    if (state.memtable.empty()) {
        state.memtable.reserve(options_.memtable_entries);
    }
    state.memtable.try_emplace(entry.id.bytes, entry.location);
    return {};
}

result<std::optional<chunk_location>> chunk_index::find(digest id) const {
    const auto state = states_.find(id.alg);
    if (state == states_.end()) {
        return std::nullopt;
    }
    if (const auto found = state->second.memtable.find(id.bytes);
        found != state->second.memtable.end()) {
        return found->second;
    }
    for (const auto& source : state->second.runs) {
        if (!bloom_may_contain(source.bloom, id.bytes)) {
            continue;
        }
        const auto bucket = (std::to_integer<unsigned char>(id.bytes[0]) << 8U) |
                            std::to_integer<unsigned char>(id.bytes[1]);
        const auto first = source.fanout[bucket];
        const auto last = source.fanout[bucket + 1];
        const auto records = source.mapping.bytes().subspan(source.records_offset);
        const auto positions = std::views::iota(first, last);
        const auto found =
            std::ranges::lower_bound(positions, id.bytes, {}, [&records](std::uint32_t position) {
                std::array<std::byte, 32> key{};
                const auto record =
                    records.subspan(std::size_t(position) * record_size, record_size);
                std::copy_n(record.begin(), key.size(), key.begin());
                return key;
            });
        if (found != positions.end()) {
            const auto record = records.subspan(std::size_t(*found) * record_size, record_size);
            if (!std::equal(record.begin(), record.begin() + 32, id.bytes.begin())) {
                continue;
            }
            return decode_record(record, id.alg).location;
        }
    }
    return std::nullopt;
}

runtime::task<void> chunk_index::flush(algorithm alg) {
    auto state_it = states_.find(alg);
    if (state_it == states_.end() || state_it->second.memtable.empty()) {
        co_return result<void>{};
    }
    auto& state = state_it->second;
    std::vector<index_entry> entries;
    entries.reserve(state.memtable.size());
    for (const auto& [id, location] : state.memtable) {
        entries.push_back({{alg, id}, location});
    }
    std::sort(
        entries.begin(), entries.end(), [](const index_entry& left, const index_entry& right) {
            return left.id.bytes < right.id.bytes;
        });
    const auto count = static_cast<std::uint64_t>(entries.size());
    const auto bloom_size = std::max<std::uint64_t>(1, (count * 10 + 7) / 8);
    // On the heap: a 256 KiB array here makes the Debug coroutine ramp reserve that much stack.
    std::vector<std::uint32_t> fanout(fanout_count);
    std::vector<std::byte> bloom(static_cast<std::size_t>(bloom_size));
    for (const auto& entry : entries) {
        const auto bucket = (std::to_integer<unsigned char>(entry.id.bytes[0]) << 8U) |
                            std::to_integer<unsigned char>(entry.id.bytes[1]);
        ++fanout[bucket + 1];
        set_bloom(bloom, entry.id.bytes);
    }
    for (std::size_t i = 1; i < fanout.size(); ++i) {
        fanout[i] += fanout[i - 1];
    }
    const auto sequence = next_sequence_++;
    const auto run_key = key("run-" + std::to_string(sequence) + ".idx");
    auto created = co_await backend_->put(run_key, make_header(alg, count, bloom_size, fanout));
    if (!created) {
        co_return std::unexpected(created.error());
    }
    auto bloom_written = co_await append_windows(backend_, run_key, run_header_size, bloom);
    if (!bloom_written) {
        co_return std::unexpected(bloom_written.error());
    }
    std::vector<std::byte> output;
    output.reserve(write_window_size);
    std::uint64_t offset = run_header_size + bloom_size;
    for (const auto& entry : entries) {
        append_record(output, entry.id.bytes, entry.location);
        if (output.size() >= write_window_size) {
            const auto size = output.size();
            auto written = co_await backend_->put_range(run_key, offset, std::move(output));
            if (!written) {
                co_return std::unexpected(written.error());
            }
            offset += size;
            output.clear();
            output.reserve(write_window_size);
        }
    }
    if (!output.empty()) {
        auto written = co_await backend_->put_range(run_key, offset, std::move(output));
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    state.memtable.clear();
    auto synchronized = co_await backend_->barrier();
    if (!synchronized) {
        co_return std::unexpected(synchronized.error());
    }
    auto mapped = co_await map_run(sequence, alg, count);
    if (!mapped) {
        co_return std::unexpected(mapped.error());
    }
    state.runs.insert(state.runs.begin(), std::move(*mapped));
    auto compacted = co_await compact(alg);
    if (!compacted) {
        co_return std::unexpected(compacted.error());
    }
    co_return co_await write_manifest();
}

runtime::task<void> chunk_index::flush() {
    for (const auto& [alg, ignored] : states_) {
        auto flushed = co_await flush(alg);
        if (!flushed) {
            co_return std::unexpected(flushed.error());
        }
    }
    co_return co_await write_manifest();
}

bool chunk_index::needs_flush(algorithm alg) const noexcept {
    const auto state = states_.find(alg);
    return state != states_.end() && state->second.memtable.size() >= options_.memtable_entries;
}

runtime::task<void> chunk_index::compact(algorithm alg) {
    auto& state = states_[alg];
    std::map<unsigned, std::vector<std::size_t>> groups;
    for (std::size_t i = 0; i < state.runs.size(); ++i) {
        groups[tier(state.runs[i].record_count)].push_back(i);
    }
    const auto group = std::find_if(groups.begin(), groups.end(), [this](const auto& item) {
        return item.second.size() > options_.compaction_runs;
    });
    if (group == groups.end()) {
        co_return result<void>{};
    }
    // The merge below suspends on every write. A concurrent flush inserts into `state.runs` and
    // reallocates it, so nothing here may point into that vector: each input is copied by value,
    // and the mapping is read through its own bytes, which a vector move does not disturb.
    std::vector<merge_input> input;
    std::set<std::uint64_t> old_sequences;
    std::uint64_t capacity = 0;
    for (const auto index : group->second) {
        const auto& source = state.runs[index];
        input.push_back({.sequence = source.sequence,
                         .record_count = source.record_count,
                         .records_offset = source.records_offset,
                         .bytes = source.mapping.bytes()});
        old_sequences.insert(source.sequence);
        capacity += source.record_count;
    }
    // Every byte of the run is written once, in order (liquid #332): an object store such as
    // Zetta's refuses a range that rewrites bytes already written. The header and bloom precede
    // the records, so a first merge over the inputs computes the count, fanout and bloom without
    // writing anything, and a second merge streams the records after them. It runs without a
    // suspension point, so no flush lands between the two merges.
    const auto bloom_size = std::max<std::uint64_t>(1, (capacity * 10 + 7) / 8);
    // On the heap, as in flush: an on-stack table makes the Debug coroutine ramp reserve its size.
    std::vector<std::uint32_t> fanout(fanout_count);
    std::vector<std::byte> bloom(static_cast<std::size_t>(bloom_size));
    std::uint64_t count = 0;
    {
        run_merge sizing(input, alg);
        while (const auto entry = sizing.next()) {
            set_bloom(bloom, entry->id.bytes);
            ++fanout[fanout_bucket(entry->id.bytes) + 1];
            ++count;
        }
    }
    for (std::size_t i = 1; i < fanout.size(); ++i) {
        fanout[i] += fanout[i - 1];
    }
    const auto sequence = next_sequence_++;
    const auto run_key = key("run-" + std::to_string(sequence) + ".idx");
    // A complete-object put replaces any run a torn earlier merge left under this unpublished key.
    auto created = co_await backend_->put(run_key, make_header(alg, count, bloom_size, fanout));
    if (!created) {
        co_return std::unexpected(created.error());
    }
    auto bloom_written = co_await append_windows(backend_, run_key, run_header_size, bloom);
    if (!bloom_written) {
        co_return std::unexpected(bloom_written.error());
    }
    std::vector<std::byte> output;
    output.reserve(write_window_size);
    std::uint64_t offset = run_header_size + bloom_size;
    std::uint64_t written_count = 0;
    run_merge records(input, alg);
    while (const auto entry = records.next()) {
        append_record(output, entry->id.bytes, entry->location);
        ++written_count;
        if (output.size() >= write_window_size) {
            const auto size = output.size();
            auto written = co_await backend_->put_range(run_key, offset, std::move(output));
            if (!written) {
                co_return std::unexpected(written.error());
            }
            offset += size;
            output.clear();
            output.reserve(write_window_size);
        }
    }
    if (!output.empty()) {
        auto written = co_await backend_->put_range(run_key, offset, std::move(output));
        if (!written) {
            co_return std::unexpected(written.error());
        }
    }
    if (written_count != count) {
        co_return std::unexpected(
            index_error(errc::internal, "index merge changed between passes", root_));
    }
    auto synchronized = co_await backend_->barrier();
    if (!synchronized) {
        co_return std::unexpected(synchronized.error());
    }
    auto mapped = co_await map_run(sequence, alg, count);
    if (!mapped) {
        co_return std::unexpected(mapped.error());
    }
    // state.runs may have grown while this coroutine suspended, so size it from the vector as it
    // is now and select by sequence, never by the indices the group was built from.
    std::vector<run> retained;
    std::vector<run> old;
    retained.reserve(state.runs.size() + 1);
    old.reserve(old_sequences.size());
    for (auto& source : state.runs) {
        if (old_sequences.contains(source.sequence)) {
            old.push_back(std::move(source));
        } else {
            retained.push_back(std::move(source));
        }
    }
    retained.push_back(std::move(*mapped));
    std::sort(retained.begin(), retained.end(), [](const run& left, const run& right) {
        return left.sequence > right.sequence;
    });
    state.runs = std::move(retained);
    auto manifest = co_await write_manifest();
    if (!manifest) {
        co_return std::unexpected(manifest.error());
    }
    for (auto& source : old) {
        source.mapping.close();
        auto removed = co_await backend_->remove(source.key);
        if (!removed && removed.error().code != errc::not_found) {
            co_return std::unexpected(removed.error());
        }
    }
    co_return result<void>{};
}

runtime::task<void> chunk_index::write_manifest() {
    std::vector<std::byte> bytes;
    bytes.insert(bytes.end(), manifest_magic_v2.begin(), manifest_magic_v2.end());
    std::uint32_t count = 0;
    for (const auto& [ignored, state] : states_) {
        count += static_cast<std::uint32_t>(state.runs.size());
    }
    put_u32(bytes, count);
    put_u32(bytes, highest_pack_id_);
    put_u64(bytes, collection_epoch_);
    for (const auto& [alg, state] : states_) {
        for (const auto& source : state.runs) {
            bytes.push_back(std::byte{static_cast<unsigned char>(alg)});
            bytes.insert(bytes.end(), 7, std::byte{0});
            put_u64(bytes, source.sequence);
            put_u64(bytes, source.record_count);
        }
    }
    put_u64(bytes, checksum(bytes));
    auto synchronized = co_await backend_->barrier();
    if (!synchronized) {
        co_return std::unexpected(synchronized.error());
    }
    auto written = co_await backend_->put(key("MANIFEST"), std::move(bytes));
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return co_await backend_->barrier();
}

runtime::task<void> chunk_index::rebuild(std::vector<index_entry> entries) {
    try {
        for (auto& [ignored, state] : states_) {
            for (auto& source : state.runs)
                source.mapping.close();
        }
        states_.clear();
        auto removed = co_await remove_backend_prefix(backend_, prefix_);
        if (!removed) {
            co_return std::unexpected(removed.error());
        }
        next_sequence_ = 1;
        for (auto& entry : entries) {
            auto inserted = insert(std::move(entry));
            if (!inserted) {
                co_return std::unexpected(inserted.error());
            }
            const auto state = states_.find(entry.id.alg);
            if (state != states_.end() &&
                state->second.memtable.size() >= options_.memtable_entries) {
                auto flushed = co_await flush(entry.id.alg);
                if (!flushed) {
                    co_return std::unexpected(flushed.error());
                }
            }
        }
        co_return co_await flush();
    } catch (const std::exception& exception) {
        co_return std::unexpected(index_error(errc::io, exception.what(), root_));
    }
}

runtime::task<void> chunk_index::close() {
    auto flushed = co_await flush();
    if (!flushed) {
        co_return std::unexpected(flushed.error());
    }
    for (auto& [ignored, state] : states_) {
        for (auto& source : state.runs) {
            source.mapping.close();
        }
    }
    co_return result<void>{};
}

} // namespace liquid::store_detail
