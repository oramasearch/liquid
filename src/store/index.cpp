#include "index.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <limits>
#include <ranges>
#include <set>
#include <string>
#include <utility>

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 8> run_magic{
    std::byte{'L'}, std::byte{'Q'}, std::byte{'I'}, std::byte{'D'},
    std::byte{'X'}, std::byte{1},   std::byte{0},   std::byte{0},
};
constexpr std::array<std::byte, 8> manifest_magic{
    std::byte{'L'}, std::byte{'Q'}, std::byte{'M'}, std::byte{'A'},
    std::byte{'N'}, std::byte{1},   std::byte{0},   std::byte{0},
};
constexpr std::size_t record_size = 48;
constexpr std::size_t fanout_count = 65'537;
constexpr std::size_t run_header_size =
    8 + 1 + 7 + 8 + 8 + fanout_count * sizeof(std::uint32_t) + 8;
constexpr std::size_t write_window_size = 4 * 1024 * 1024;

error index_error(errc code, std::string message, const std::filesystem::path &path) {
  return {code, std::move(message), path.string()};
}

void put_u32(std::vector<std::byte> &out, std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i) {
    out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
  }
}

void put_u64(std::vector<std::byte> &out, std::uint64_t value) {
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

std::uint64_t bloom_hash(const std::array<std::byte, 32> &id) { return checksum(id); }

void set_bloom(std::vector<std::byte> &bloom, const std::array<std::byte, 32> &id) {
  const auto bit_count = bloom.size() * 8;
  const auto hash = bloom_hash(id);
  for (unsigned i = 0; i != 7; ++i) {
    const auto bit = (hash + i * 0x9e3779b97f4a7c15ULL) % bit_count;
    bloom[bit / 8] |= std::byte{static_cast<unsigned char>(1U << (bit % 8))};
  }
}

bool bloom_may_contain(std::span<const std::byte> bloom, const std::array<std::byte, 32> &id) {
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

void append_record(std::vector<std::byte> &bytes, const std::array<std::byte, 32> &id,
                   chunk_location location) {
  bytes.insert(bytes.end(), id.begin(), id.end());
  put_u32(bytes, location.pack_id);
  put_u64(bytes, location.offset);
  put_u32(bytes, location.length);
}

std::vector<std::byte> make_header(algorithm alg, std::uint64_t count, std::uint64_t bloom_size,
                                   const std::array<std::uint32_t, fanout_count> &fanout) {
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

} // namespace

chunk_index::chunk_index(std::filesystem::path root, chunk_index_options options)
    : root_(std::move(root)), options_(options) {
  options_.memtable_entries = std::max<std::size_t>(1, options_.memtable_entries);
  options_.compaction_runs = std::max<std::size_t>(1, options_.compaction_runs);
}

std::size_t
chunk_index::digest_hash::operator()(const std::array<std::byte, 32> &value) const noexcept {
  return static_cast<std::size_t>(bloom_hash(value));
}

result<chunk_index::run> chunk_index::map_run(std::uint64_t sequence, algorithm alg,
                                              std::uint64_t expected_count) const {
  run output{};
  output.sequence = sequence;
  output.algorithm_value = alg;
  output.path = root_ / ("run-" + std::to_string(sequence) + ".idx");
  auto mapped = output.mapping.open(output.path);
  if (!mapped) {
    return std::unexpected(mapped.error());
  }
  const auto bytes = output.mapping.bytes();
  if (bytes.size() < run_header_size ||
      !std::equal(run_magic.begin(), run_magic.end(), bytes.begin()) ||
      std::to_integer<unsigned char>(bytes[8]) != static_cast<unsigned char>(alg) ||
      get_u64(bytes, 16) != expected_count ||
      checksum(bytes.first(run_header_size - 8)) != get_u64(bytes, run_header_size - 8)) {
    return std::unexpected(index_error(errc::corrupt, "corrupt index run header", output.path));
  }
  const auto bloom_size = get_u64(bytes, 24);
  if (bloom_size > std::numeric_limits<std::size_t>::max() ||
      bytes.size() != run_header_size + bloom_size + expected_count * record_size) {
    return std::unexpected(index_error(errc::corrupt, "invalid index run size", output.path));
  }
  output.record_count = expected_count;
  output.fanout = std::span<const std::uint32_t>(
      reinterpret_cast<const std::uint32_t *>(bytes.data() + 32), fanout_count);
  if (output.fanout.front() != 0 || output.fanout.back() != expected_count ||
      !std::is_sorted(output.fanout.begin(), output.fanout.end())) {
    return std::unexpected(index_error(errc::corrupt, "invalid index fanout", output.path));
  }
  output.bloom = bytes.subspan(run_header_size, static_cast<std::size_t>(bloom_size));
  output.records_offset = run_header_size + bloom_size;
  return output;
}

runtime::task<void> chunk_index::open() {
  try {
    states_.clear();
    next_sequence_ = 1;
    std::filesystem::create_directories(root_);
    const auto manifest = root_ / "MANIFEST";
    if (!std::filesystem::exists(manifest)) {
      co_return result<void>{};
    }
    auto raw = co_await read_file(manifest);
    if (!raw) {
      co_return std::unexpected(raw.error());
    }
    const auto bytes = std::span<const std::byte>(*raw);
    if (bytes.size() < 24 ||
        !std::equal(manifest_magic.begin(), manifest_magic.end(), bytes.begin())) {
      co_return std::unexpected(index_error(errc::corrupt, "invalid index manifest", manifest));
    }
    const auto count = get_u32(bytes, 8);
    highest_pack_id_ = get_u32(bytes, 12);
    if (bytes.size() != 16 + std::size_t(count) * 24 + 8 ||
        checksum(bytes.first(bytes.size() - 8)) != get_u64(bytes, bytes.size() - 8)) {
      co_return std::unexpected(index_error(errc::corrupt, "corrupt index manifest", manifest));
    }
    for (std::size_t i = 0, offset = 16; i < count; ++i, offset += 24) {
      const auto algorithm_byte = std::to_integer<unsigned char>(bytes[offset]);
      if (!valid_algorithm(algorithm_byte)) {
        co_return std::unexpected(index_error(errc::corrupt, "unknown run algorithm", manifest));
      }
      const auto sequence = get_u64(bytes, offset + 8);
      auto mapped =
          map_run(sequence, static_cast<algorithm>(algorithm_byte), get_u64(bytes, offset + 16));
      if (!mapped) {
        co_return std::unexpected(mapped.error());
      }
      states_[mapped->algorithm_value].runs.push_back(std::move(*mapped));
      next_sequence_ = std::max(next_sequence_, sequence + 1);
    }
    for (auto &[ignored, state] : states_) {
      std::sort(state.runs.begin(), state.runs.end(),
                [](const run &left, const run &right) { return left.sequence > right.sequence; });
    }
    co_return result<void>{};
  } catch (const std::exception &exception) {
    co_return std::unexpected(index_error(errc::io, exception.what(), root_));
  }
}

result<void> chunk_index::insert(index_entry entry) {
  auto &state = states_[entry.id.alg];
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
  for (const auto &source : state->second.runs) {
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
          const auto record = records.subspan(std::size_t(position) * record_size, record_size);
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
  auto &state = state_it->second;
  std::vector<index_entry> entries;
  entries.reserve(state.memtable.size());
  for (const auto &[id, location] : state.memtable) {
    entries.push_back({{alg, id}, location});
  }
  std::sort(entries.begin(), entries.end(), [](const index_entry &left, const index_entry &right) {
    return left.id.bytes < right.id.bytes;
  });
  const auto count = static_cast<std::uint64_t>(entries.size());
  const auto bloom_size = std::max<std::uint64_t>(1, (count * 10 + 7) / 8);
  std::array<std::uint32_t, fanout_count> fanout{};
  std::vector<std::byte> bloom(static_cast<std::size_t>(bloom_size));
  for (const auto &entry : entries) {
    const auto bucket = (std::to_integer<unsigned char>(entry.id.bytes[0]) << 8U) |
                        std::to_integer<unsigned char>(entry.id.bytes[1]);
    ++fanout[bucket + 1];
    set_bloom(bloom, entry.id.bytes);
  }
  for (std::size_t i = 1; i < fanout.size(); ++i) {
    fanout[i] += fanout[i - 1];
  }
  const auto sequence = next_sequence_++;
  const auto path = root_ / ("run-" + std::to_string(sequence) + ".idx");
  auto created = co_await write_file(path, make_header(alg, count, bloom_size, fanout));
  if (!created) {
    co_return std::unexpected(created.error());
  }
  auto bloom_written = co_await write_file_range(path, run_header_size, bloom);
  if (!bloom_written) {
    co_return std::unexpected(bloom_written.error());
  }
  std::vector<std::byte> output;
  output.reserve(write_window_size);
  std::uint64_t offset = run_header_size + bloom_size;
  for (const auto &entry : entries) {
    append_record(output, entry.id.bytes, entry.location);
    if (output.size() >= write_window_size) {
      const auto size = output.size();
      auto written = co_await write_file_range(path, offset, std::move(output));
      if (!written) {
        co_return std::unexpected(written.error());
      }
      offset += size;
      output.clear();
      output.reserve(write_window_size);
    }
  }
  if (!output.empty()) {
    auto written = co_await write_file_range(path, offset, std::move(output));
    if (!written) {
      co_return std::unexpected(written.error());
    }
  }
  state.memtable.clear();
  auto mapped = map_run(sequence, alg, count);
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
  for (const auto &[alg, ignored] : states_) {
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
  auto &state = states_[alg];
  std::map<unsigned, std::vector<std::size_t>> groups;
  for (std::size_t i = 0; i < state.runs.size(); ++i) {
    groups[tier(state.runs[i].record_count)].push_back(i);
  }
  const auto group = std::find_if(groups.begin(), groups.end(), [this](const auto &item) {
    return item.second.size() > options_.compaction_runs;
  });
  if (group == groups.end()) {
    co_return result<void>{};
  }
  std::vector<const run *> input;
  std::set<std::uint64_t> old_sequences;
  std::uint64_t capacity = 0;
  for (const auto index : group->second) {
    input.push_back(&state.runs[index]);
    old_sequences.insert(state.runs[index].sequence);
    capacity += state.runs[index].record_count;
  }
  struct cursor {
    const run *source{};
    std::uint64_t position{};
  };
  std::vector<cursor> cursors;
  std::vector<std::optional<index_entry>> heads;
  for (const auto *source : input) {
    cursors.push_back({source, 0});
    heads.push_back(
        decode_record(source->mapping.bytes().subspan(source->records_offset, record_size), alg));
  }
  const auto bloom_size = std::max<std::uint64_t>(1, (capacity * 10 + 7) / 8);
  std::array<std::uint32_t, fanout_count> fanout{};
  std::vector<std::byte> bloom(static_cast<std::size_t>(bloom_size));
  const auto sequence = next_sequence_++;
  const auto path = root_ / ("run-" + std::to_string(sequence) + ".idx");
  std::array<std::uint32_t, fanout_count> empty_fanout{};
  auto created = co_await write_file(path, make_header(alg, 0, bloom_size, empty_fanout));
  if (!created) {
    co_return std::unexpected(created.error());
  }
  auto bloom_space = co_await write_file_range(path, run_header_size, bloom);
  if (!bloom_space) {
    co_return std::unexpected(bloom_space.error());
  }
  std::vector<std::byte> output;
  output.reserve(write_window_size);
  std::uint64_t offset = run_header_size + bloom_size;
  std::uint64_t count = 0;
  while (true) {
    std::optional<std::size_t> choice;
    for (std::size_t i = 0; i < heads.size(); ++i) {
      if (heads[i] && (!choice || heads[i]->id.bytes < heads[*choice]->id.bytes)) {
        choice = i;
      }
    }
    if (!choice) {
      break;
    }
    const auto key = heads[*choice]->id.bytes;
    std::size_t newest = *choice;
    for (std::size_t i = 0; i < heads.size(); ++i) {
      if (heads[i] && heads[i]->id.bytes == key && input[i]->sequence > input[newest]->sequence) {
        newest = i;
      }
    }
    append_record(output, key, heads[newest]->location);
    set_bloom(bloom, key);
    const auto bucket =
        (std::to_integer<unsigned char>(key[0]) << 8U) | std::to_integer<unsigned char>(key[1]);
    ++fanout[bucket + 1];
    ++count;
    if (output.size() >= write_window_size) {
      const auto size = output.size();
      auto written = co_await write_file_range(path, offset, std::move(output));
      if (!written) {
        co_return std::unexpected(written.error());
      }
      offset += size;
      output.clear();
      output.reserve(write_window_size);
    }
    for (std::size_t i = 0; i < heads.size(); ++i) {
      if (!heads[i] || heads[i]->id.bytes != key) {
        continue;
      }
      ++cursors[i].position;
      if (cursors[i].position == input[i]->record_count) {
        heads[i] = std::nullopt;
      } else {
        heads[i] = decode_record(
            input[i]->mapping.bytes().subspan(
                input[i]->records_offset + cursors[i].position * record_size, record_size),
            alg);
      }
    }
  }
  if (!output.empty()) {
    auto written = co_await write_file_range(path, offset, std::move(output));
    if (!written) {
      co_return std::unexpected(written.error());
    }
  }
  for (std::size_t i = 1; i < fanout.size(); ++i) {
    fanout[i] += fanout[i - 1];
  }
  auto header_written =
      co_await write_file_range(path, 0, make_header(alg, count, bloom_size, fanout));
  if (!header_written) {
    co_return std::unexpected(header_written.error());
  }
  auto bloom_written = co_await write_file_range(path, run_header_size, bloom);
  if (!bloom_written) {
    co_return std::unexpected(bloom_written.error());
  }
  auto mapped = map_run(sequence, alg, count);
  if (!mapped) {
    co_return std::unexpected(mapped.error());
  }
  std::vector<run> retained;
  std::vector<run> old;
  retained.reserve(state.runs.size() - input.size() + 1);
  old.reserve(input.size());
  for (auto &source : state.runs) {
    if (old_sequences.contains(source.sequence)) {
      old.push_back(std::move(source));
    } else {
      retained.push_back(std::move(source));
    }
  }
  retained.push_back(std::move(*mapped));
  std::sort(retained.begin(), retained.end(),
            [](const run &left, const run &right) { return left.sequence > right.sequence; });
  state.runs = std::move(retained);
  auto manifest = co_await write_manifest();
  if (!manifest) {
    co_return std::unexpected(manifest.error());
  }
  for (auto &source : old) {
    source.mapping.close();
    std::error_code ignored;
    std::filesystem::remove(source.path, ignored);
  }
  co_return result<void>{};
}

runtime::task<void> chunk_index::write_manifest() {
  std::vector<std::byte> bytes;
  bytes.insert(bytes.end(), manifest_magic.begin(), manifest_magic.end());
  std::uint32_t count = 0;
  for (const auto &[ignored, state] : states_) {
    count += static_cast<std::uint32_t>(state.runs.size());
  }
  put_u32(bytes, count);
  put_u32(bytes, highest_pack_id_);
  for (const auto &[alg, state] : states_) {
    for (const auto &source : state.runs) {
      bytes.push_back(std::byte{static_cast<unsigned char>(alg)});
      bytes.insert(bytes.end(), 7, std::byte{0});
      put_u64(bytes, source.sequence);
      put_u64(bytes, source.record_count);
    }
  }
  put_u64(bytes, checksum(bytes));
  const auto temporary = root_ / "MANIFEST.tmp";
  auto written = co_await write_file(temporary, std::move(bytes));
  if (!written) {
    co_return std::unexpected(written.error());
  }
  try {
    std::filesystem::rename(temporary, root_ / "MANIFEST");
  } catch (const std::exception &exception) {
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
    for (auto &entry : entries) {
      auto inserted = insert(std::move(entry));
      if (!inserted) {
        co_return std::unexpected(inserted.error());
      }
      const auto state = states_.find(entry.id.alg);
      if (state != states_.end() && state->second.memtable.size() >= options_.memtable_entries) {
        auto flushed = co_await flush(entry.id.alg);
        if (!flushed) {
          co_return std::unexpected(flushed.error());
        }
      }
    }
    co_return co_await flush();
  } catch (const std::exception &exception) {
    co_return std::unexpected(index_error(errc::io, exception.what(), root_));
  }
}

runtime::task<void> chunk_index::close() {
  auto flushed = co_await flush();
  if (!flushed) {
    co_return std::unexpected(flushed.error());
  }
  for (auto &[ignored, state] : states_) {
    for (auto &source : state.runs) {
      source.mapping.close();
    }
  }
  co_return result<void>{};
}

} // namespace liquid::store_detail
