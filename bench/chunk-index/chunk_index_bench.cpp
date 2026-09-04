#include <rocksdb/cache.h>
#include <rocksdb/db.h>
#include <rocksdb/filter_policy.h>
#include <rocksdb/options.h>
#include <rocksdb/slice.h>
#include <rocksdb/statistics.h>
#include <rocksdb/table.h>
#include <rocksdb/write_batch.h>

extern "C" {
#include <lmdb.h>
}

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <format>
#include <iostream>
#include <limits>
#include <memory>
#include <random>
#include <stdexcept>
#include <string>
#include <string_view>
#include <system_error>
#include <utility>
#include <vector>

#include "index.hpp"

#include <sys/resource.h>

#if defined(__linux__)
#include <fcntl.h>
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;
using clock_type = std::chrono::steady_clock;
using key_type = std::array<char, 32>;
using value_type = std::array<char, 16>;

constexpr std::uint64_t seed = 1;
constexpr std::size_t batch_size = 10'000;
constexpr std::size_t point_lookup_count = 1'000'000;
constexpr std::size_t range_scan_count = 10'000;
constexpr std::size_t range_scan_width = 64;
constexpr std::size_t cold_lookup_count = 100'000;
constexpr std::uint64_t mib = 1024ULL * 1024ULL;

struct config {
  std::uint64_t n = 0;
  std::string engine;
  fs::path directory;
  std::string tuning = "default";
  std::uint64_t lmdb_map_mib = 0;
};

struct query {
  key_type key;
  bool expect_hit;
};

struct phase_result {
  std::string name;
  std::uint64_t operations;
  double seconds;
  double p50_us;
  double p99_us;
};

[[noreturn]] void fail(std::string_view message) {
  throw std::runtime_error(std::string(message));
}

std::uint64_t parse_u64(std::string_view value, std::string_view option) {
  std::uint64_t parsed = 0;
  try {
    std::size_t used = 0;
    parsed = std::stoull(std::string(value), &used);
    if (used != value.size()) {
      fail(std::format("invalid value for {}: {}", option, value));
    }
  } catch (const std::exception &) {
    fail(std::format("invalid value for {}: {}", option, value));
  }
  return parsed;
}

config parse_arguments(int argc, char **argv) {
  config result;
  for (int i = 1; i < argc; ++i) {
    const std::string_view argument(argv[i]);
    const auto take_value = [&](std::string_view option) -> std::string_view {
      if (++i >= argc) {
        fail(std::format("{} requires a value", option));
      }
      return argv[i];
    };

    if (argument == "--n") {
      result.n = parse_u64(take_value(argument), argument);
    } else if (argument == "--engine") {
      result.engine = take_value(argument);
    } else if (argument == "--dir") {
      result.directory = take_value(argument);
    } else if (argument == "--tuning") {
      result.tuning = take_value(argument);
    } else if (argument == "--lmdb-map-mib") {
      result.lmdb_map_mib = parse_u64(take_value(argument), argument);
    } else if (argument == "--help") {
      std::cout
          << "usage: chunk-index-bench --n N --engine rocksdb|lmdb|liquid --dir PATH "
             "[--tuning default|tuned|nosync] [--lmdb-map-mib MIB]\n";
      std::exit(0);
    } else {
      fail(std::format("unknown argument: {}", argument));
    }
  }

  if (result.n < range_scan_width) {
    fail("--n must be at least 64");
  }
  if (result.engine != "rocksdb" && result.engine != "lmdb" &&
      result.engine != "liquid") {
    fail("--engine must be rocksdb, lmdb, or liquid");
  }
  if (result.directory.empty()) {
    fail("--dir is required");
  }
  if (result.engine == "rocksdb" && result.tuning != "default" &&
      result.tuning != "tuned") {
    fail("RocksDB tuning must be default or tuned");
  }
  if (result.engine == "lmdb" && result.tuning != "default" &&
      result.tuning != "nosync") {
    fail("LMDB tuning must be default or nosync");
  }
  return result;
}

key_type next_key(std::mt19937_64 &random) {
  key_type key{};
  for (std::size_t word = 0; word < 4; ++word) {
    const std::uint64_t bits = random();
    for (std::size_t byte = 0; byte < 8; ++byte) {
      key[word * 8 + byte] = static_cast<char>((bits >> (byte * 8)) & 0xffU);
    }
  }
  return key;
}

void store_u32(value_type &value, std::size_t at, std::uint32_t number) {
  for (std::size_t byte = 0; byte < 4; ++byte) {
    value[at + byte] = static_cast<char>((number >> (byte * 8)) & 0xffU);
  }
}

void store_u64(value_type &value, std::size_t at, std::uint64_t number) {
  for (std::size_t byte = 0; byte < 8; ++byte) {
    value[at + byte] = static_cast<char>((number >> (byte * 8)) & 0xffU);
  }
}

value_type make_value(std::uint64_t index) {
  constexpr std::uint64_t chunks_per_pack = 512;
  constexpr std::uint32_t chunk_length = 64 * 1024;
  value_type value{};
  store_u32(value, 0, static_cast<std::uint32_t>(index / chunks_per_pack));
  store_u64(value, 4, (index % chunks_per_pack) * chunk_length);
  store_u32(value, 12, chunk_length);
  return value;
}

rocksdb::Slice as_slice(const key_type &key) {
  return {key.data(), key.size()};
}

std::pair<double, double>
percentiles(std::vector<std::uint64_t> &latencies_ns) {
  if (latencies_ns.empty()) {
    return {0.0, 0.0};
  }
  std::sort(latencies_ns.begin(), latencies_ns.end());
  const auto pick = [&](double percentile) {
    const auto position = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(latencies_ns.size())) - 1.0);
    return static_cast<double>(
               latencies_ns[std::min(position, latencies_ns.size() - 1)]) /
           1'000.0;
  };
  return {pick(0.50), pick(0.99)};
}

phase_result finish_phase(std::string name, std::uint64_t operations,
                          clock_type::time_point started,
                          std::vector<std::uint64_t> &latencies_ns) {
  const auto ended = clock_type::now();
  const double seconds = std::chrono::duration<double>(ended - started).count();
  const auto [p50, p99] = percentiles(latencies_ns);
  return {std::move(name), operations, seconds, p50, p99};
}

void check_rocks(const rocksdb::Status &status, std::string_view operation) {
  if (!status.ok()) {
    fail(std::format("RocksDB {} failed: {}", operation, status.ToString()));
  }
}

void check_lmdb(int status, std::string_view operation) {
  if (status != MDB_SUCCESS) {
    fail(std::format("LMDB {} failed: {}", operation, mdb_strerror(status)));
  }
}

class engine {
public:
  virtual ~engine() = default;
  virtual void open(bool create) = 0;
  virtual void put_batch(const std::vector<key_type> &keys, std::uint64_t first,
                         std::uint64_t count) = 0;
  virtual void begin_reads() = 0;
  virtual bool get(const key_type &key) = 0;
  virtual std::size_t scan(const key_type &key, std::size_t count) = 0;
  virtual void end_reads() = 0;
  virtual void flush() = 0;
  virtual void close() = 0;
  virtual std::uint64_t bytes_written() const = 0;
  virtual std::string details() const = 0;
};

class rocks_engine final : public engine {
public:
  rocks_engine(fs::path directory, bool tuned)
      : directory_(std::move(directory)), tuned_(tuned),
        statistics_(rocksdb::CreateDBStatistics()) {}

  void open(bool create) override {
    rocksdb::Options options;
    options.create_if_missing = create;
    options.error_if_exists = create;
    options.compression = rocksdb::kNoCompression;
    options.statistics = statistics_;
    if (tuned_) {
      rocksdb::BlockBasedTableOptions table;
      table.filter_policy.reset(rocksdb::NewBloomFilterPolicy(10, false));
      table.block_cache = rocksdb::NewLRUCache(512 * mib);
      options.table_factory.reset(rocksdb::NewBlockBasedTableFactory(table));
    }

    rocksdb::DB *raw = nullptr;
    check_rocks(rocksdb::DB::Open(options, directory_.string(), &raw), "open");
    database_.reset(raw);
  }

  void put_batch(const std::vector<key_type> &keys, std::uint64_t first,
                 std::uint64_t count) override {
    rocksdb::WriteBatch batch;
    for (std::uint64_t i = first; i < first + count; ++i) {
      const auto value = make_value(i);
      batch.Put(as_slice(keys[static_cast<std::size_t>(i)]),
                rocksdb::Slice(value.data(), value.size()));
    }
    check_rocks(database_->Write(rocksdb::WriteOptions{}, &batch),
                "write batch");
  }

  void begin_reads() override {}

  bool get(const key_type &key) override {
    rocksdb::PinnableSlice value;
    const auto status =
        database_->Get(rocksdb::ReadOptions{}, database_->DefaultColumnFamily(),
                       as_slice(key), &value);
    if (status.IsNotFound()) {
      return false;
    }
    check_rocks(status, "get");
    if (value.size() != value_type{}.size()) {
      fail("RocksDB returned a value with the wrong size");
    }
    return true;
  }

  std::size_t scan(const key_type &key, std::size_t count) override {
    std::unique_ptr<rocksdb::Iterator> iterator(
        database_->NewIterator(rocksdb::ReadOptions{}));
    iterator->Seek(as_slice(key));
    std::size_t visited = 0;
    bool wrapped = false;
    while (visited < count) {
      if (!iterator->Valid()) {
        check_rocks(iterator->status(), "range scan");
        if (wrapped) {
          break;
        }
        iterator->SeekToFirst();
        wrapped = true;
        continue;
      }
      ++visited;
      iterator->Next();
    }
    check_rocks(iterator->status(), "range scan");
    return visited;
  }

  void end_reads() override {}

  void flush() override {
    rocksdb::FlushOptions options;
    options.wait = true;
    check_rocks(database_->Flush(options), "flush");
  }

  void close() override { database_.reset(); }

  std::uint64_t bytes_written() const override {
    return statistics_->getTickerCount(rocksdb::Tickers::BYTES_WRITTEN);
  }

  std::string details() const override {
    return tuned_ ? "bloom_bits=10 block_cache_mib=512 compression=none"
                  : "bloom_bits=default block_cache=default compression=none";
  }

private:
  fs::path directory_;
  bool tuned_;
  std::shared_ptr<rocksdb::Statistics> statistics_;
  std::unique_ptr<rocksdb::DB> database_;
};

class lmdb_engine final : public engine {
public:
  lmdb_engine(fs::path directory, std::uint64_t map_size, bool nosync)
      : directory_(std::move(directory)), map_size_(map_size), nosync_(nosync) {
  }

  ~lmdb_engine() override { close(); }

  void open(bool create) override {
    check_lmdb(mdb_env_create(&environment_), "environment create");
    check_lmdb(
        mdb_env_set_mapsize(environment_, static_cast<std::size_t>(map_size_)),
        "set map size");
    const unsigned int flags = nosync_ ? MDB_NOSYNC : 0;
    check_lmdb(mdb_env_open(environment_, directory_.c_str(), flags, 0644),
               "environment open");

    MDB_txn *transaction = nullptr;
    check_lmdb(mdb_txn_begin(environment_, nullptr, create ? 0 : MDB_RDONLY,
                             &transaction),
               "database transaction begin");
    const int open_status =
        mdb_dbi_open(transaction, nullptr, create ? MDB_CREATE : 0, &database_);
    if (open_status != MDB_SUCCESS) {
      mdb_txn_abort(transaction);
      check_lmdb(open_status, "database open");
    }
    const int finish_status = create
                                  ? mdb_txn_commit(transaction)
                                  : (mdb_txn_abort(transaction), MDB_SUCCESS);
    check_lmdb(finish_status, "database transaction commit");
  }

  void put_batch(const std::vector<key_type> &keys, std::uint64_t first,
                 std::uint64_t count) override {
    MDB_txn *transaction = nullptr;
    check_lmdb(mdb_txn_begin(environment_, nullptr, 0, &transaction),
               "write transaction begin");
    for (std::uint64_t i = first; i < first + count; ++i) {
      auto &key_bytes = keys[static_cast<std::size_t>(i)];
      auto value_bytes = make_value(i);
      MDB_val key{key_bytes.size(), const_cast<char *>(key_bytes.data())};
      MDB_val value{value_bytes.size(), value_bytes.data()};
      const int status = mdb_put(transaction, database_, &key, &value, 0);
      if (status != MDB_SUCCESS) {
        mdb_txn_abort(transaction);
        check_lmdb(status, "put");
      }
    }
    check_lmdb(mdb_txn_commit(transaction), "write transaction commit");
  }

  void begin_reads() override {
    if (read_transaction_ != nullptr) {
      fail("LMDB read transaction already active");
    }
    check_lmdb(
        mdb_txn_begin(environment_, nullptr, MDB_RDONLY, &read_transaction_),
        "read transaction begin");
  }

  bool get(const key_type &key_bytes) override {
    MDB_val key{key_bytes.size(), const_cast<char *>(key_bytes.data())};
    MDB_val value{};
    const int status = mdb_get(read_transaction_, database_, &key, &value);
    if (status == MDB_NOTFOUND) {
      return false;
    }
    check_lmdb(status, "get");
    if (value.mv_size != value_type{}.size()) {
      fail("LMDB returned a value with the wrong size");
    }
    return true;
  }

  std::size_t scan(const key_type &key_bytes, std::size_t count) override {
    MDB_cursor *cursor = nullptr;
    check_lmdb(mdb_cursor_open(read_transaction_, database_, &cursor),
               "cursor open");
    MDB_val key{key_bytes.size(), const_cast<char *>(key_bytes.data())};
    MDB_val value{};
    int status = mdb_cursor_get(cursor, &key, &value, MDB_SET_RANGE);
    std::size_t visited = 0;
    bool wrapped = false;
    while (visited < count) {
      if (status == MDB_NOTFOUND) {
        if (wrapped) {
          break;
        }
        status = mdb_cursor_get(cursor, &key, &value, MDB_FIRST);
        wrapped = true;
        continue;
      }
      if (status != MDB_SUCCESS) {
        mdb_cursor_close(cursor);
        check_lmdb(status, "range scan");
      }
      ++visited;
      status = mdb_cursor_get(cursor, &key, &value, MDB_NEXT);
    }
    mdb_cursor_close(cursor);
    if (status != MDB_SUCCESS && status != MDB_NOTFOUND) {
      check_lmdb(status, "range scan");
    }
    return visited;
  }

  void end_reads() override {
    if (read_transaction_ != nullptr) {
      mdb_txn_abort(read_transaction_);
      read_transaction_ = nullptr;
    }
  }

  void flush() override {
    check_lmdb(mdb_env_sync(environment_, 1), "environment sync");
  }

  void close() override {
    end_reads();
    if (environment_ != nullptr) {
      mdb_dbi_close(environment_, database_);
      mdb_env_close(environment_);
      environment_ = nullptr;
      database_ = 0;
    }
  }

  std::uint64_t bytes_written() const override { return 0; }

  std::string details() const override {
    return std::format("map_mib={} nosync={}", map_size_ / mib,
                       nosync_ ? "on" : "off");
  }

private:
  fs::path directory_;
  std::uint64_t map_size_;
  bool nosync_;
  MDB_env *environment_ = nullptr;
  MDB_dbi database_ = 0;
  MDB_txn *read_transaction_ = nullptr;
};

std::uint64_t directory_size(const fs::path &directory) {
  std::uint64_t total = 0;
  for (const auto &entry : fs::recursive_directory_iterator(directory)) {
    if (entry.is_regular_file()) {
      total += entry.file_size();
    }
  }
  return total;
}

std::string drop_page_cache(const fs::path &directory) {
#if defined(__linux__)
  std::size_t advised = 0;
  for (const auto &entry : fs::recursive_directory_iterator(directory)) {
    if (!entry.is_regular_file()) {
      continue;
    }
    const int descriptor = ::open(entry.path().c_str(), O_RDONLY);
    if (descriptor < 0) {
      return "linux_posix_fadvise_failed";
    }
    const int status = ::posix_fadvise(descriptor, 0, 0, POSIX_FADV_DONTNEED);
    ::close(descriptor);
    if (status != 0) {
      return "linux_posix_fadvise_failed";
    }
    ++advised;
  }
  return std::format("linux_posix_fadvise_files={}", advised);
#elif defined(__APPLE__)
  static_cast<void>(directory);
  return "unavailable_without_root_on_macos";
#else
  static_cast<void>(directory);
  return "unsupported_on_this_platform";
#endif
}

std::uint64_t peak_rss_bytes() {
  rusage usage{};
  if (getrusage(RUSAGE_SELF, &usage) != 0) {
    fail("getrusage failed");
  }
#if defined(__APPLE__)
  return static_cast<std::uint64_t>(usage.ru_maxrss);
#else
  return static_cast<std::uint64_t>(usage.ru_maxrss) * 1024ULL;
#endif
}

void prepare_directory(const fs::path &directory) {
  const fs::path absolute = fs::absolute(directory).lexically_normal();
  if (absolute == absolute.root_path()) {
    fail("refusing to use a filesystem root as --dir");
  }
  if (fs::exists(absolute)) {
    if (!fs::is_directory(absolute) || !fs::is_empty(absolute)) {
      fail("--dir must not exist or must be an empty directory");
    }
  } else if (!fs::create_directories(absolute)) {
    fail("could not create --dir");
  }
}

std::uint64_t lmdb_map_size(const config &settings) {
  if (settings.lmdb_map_mib != 0) {
    if (settings.lmdb_map_mib >
        std::numeric_limits<std::uint64_t>::max() / mib) {
      fail("--lmdb-map-mib is too large");
    }
    return settings.lmdb_map_mib * mib;
  }
  if (settings.n >
      (std::numeric_limits<std::uint64_t>::max() - 256 * mib) / 256) {
    fail("--n is too large to derive an LMDB map size");
  }
  return std::max(512 * mib, settings.n * 256 + 256 * mib);
}

std::vector<key_type> generate_keys(std::uint64_t count) {
  if (count > std::numeric_limits<std::size_t>::max()) {
    fail("--n exceeds addressable memory");
  }
  std::vector<key_type> keys;
  keys.reserve(static_cast<std::size_t>(count));
  std::mt19937_64 random(seed);
  for (std::uint64_t i = 0; i < count; ++i) {
    keys.push_back(next_key(random));
  }
  return keys;
}

std::vector<query> generate_queries(const std::vector<key_type> &keys,
                                    std::size_t count) {
  std::vector<query> queries;
  queries.reserve(count);
  std::mt19937_64 random(2);
  for (std::size_t i = 0; i < count; ++i) {
    if ((i % 2) == 0) {
      queries.push_back(
          {keys[static_cast<std::size_t>(random() % keys.size())], true});
    } else {
      queries.push_back({next_key(random), false});
    }
  }
  return queries;
}

phase_result run_inserts(engine &database, const std::vector<key_type> &keys) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve((keys.size() + batch_size - 1) / batch_size);
  const auto phase_started = clock_type::now();
  for (std::uint64_t first = 0; first < keys.size(); first += batch_size) {
    const std::uint64_t count =
        std::min<std::uint64_t>(batch_size, keys.size() - first);
    const auto started = clock_type::now();
    database.put_batch(keys, first, count);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock_type::now() - started);
    latencies.push_back(static_cast<std::uint64_t>(elapsed.count()) / count);
  }
  return finish_phase("bulk_insert", keys.size(), phase_started, latencies);
}

phase_result run_points(engine &database, const std::vector<query> &queries,
                        std::size_t count, std::string name) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve(count);
  database.begin_reads();
  const auto phase_started = clock_type::now();
  for (std::size_t i = 0; i < count; ++i) {
    const auto started = clock_type::now();
    const bool found = database.get(queries[i].key);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock_type::now() - started);
    latencies.push_back(static_cast<std::uint64_t>(elapsed.count()));
    if (found != queries[i].expect_hit) {
      database.end_reads();
      fail(std::format("{} lookup {} returned the wrong presence result", name,
                       i));
    }
  }
  database.end_reads();
  return finish_phase(std::move(name), count, phase_started, latencies);
}

phase_result run_ranges(engine &database, const std::vector<key_type> &keys) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve(range_scan_count);
  std::mt19937_64 random(3);
  database.begin_reads();
  const auto phase_started = clock_type::now();
  for (std::size_t i = 0; i < range_scan_count; ++i) {
    const auto &key = keys[static_cast<std::size_t>(random() % keys.size())];
    const auto started = clock_type::now();
    const std::size_t visited = database.scan(key, range_scan_width);
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(
        clock_type::now() - started);
    latencies.push_back(static_cast<std::uint64_t>(elapsed.count()));
    if (visited != range_scan_width) {
      database.end_reads();
      fail(std::format("range scan {} visited {} entries instead of {}", i,
                       visited, range_scan_width));
    }
  }
  database.end_reads();
  return finish_phase("range_scan_64", range_scan_count, phase_started,
                      latencies);
}

void print_phase(const phase_result &phase);

liquid::digest liquid_digest(const key_type &key) {
  liquid::digest output{liquid::algorithm::blake3, {}};
  for (std::size_t i = 0; i < key.size(); ++i)
    output.bytes[i] = std::byte{static_cast<unsigned char>(key[i])};
  return output;
}

liquid::runtime::task<phase_result>
run_liquid_inserts(liquid::store_detail::chunk_index &index,
                   const std::vector<key_type> &keys) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve((keys.size() + batch_size - 1) / batch_size);
  const auto phase_started = clock_type::now();
  for (std::uint64_t first = 0; first < keys.size(); first += batch_size) {
    const auto batch_started = clock_type::now();
    const auto count = std::min<std::uint64_t>(batch_size, keys.size() - first);
    for (std::uint64_t i = first; i < first + count; ++i) {
      liquid::store_detail::chunk_location location{};
      location.pack_id = static_cast<std::uint32_t>(i / 512);
      location.offset = (i % 512) * 64U * 1024U;
      location.length = 64U * 1024U;
      const auto id = liquid_digest(keys[static_cast<std::size_t>(i)]);
      auto inserted = index.insert({id, location});
      if (!inserted) co_return std::unexpected(inserted.error());
      if (index.needs_flush(id.alg)) {
        auto flushed = co_await index.flush();
        if (!flushed) co_return std::unexpected(flushed.error());
      }
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - batch_started);
    latencies.push_back(static_cast<std::uint64_t>(elapsed.count()) / count);
  }
  co_return finish_phase("bulk_insert", keys.size(), phase_started, latencies);
}

liquid::runtime::task<phase_result>
run_liquid_points(liquid::store_detail::chunk_index &index,
                  const std::vector<query> &queries, std::size_t count,
                  std::string name) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve(count);
  const auto phase_started = clock_type::now();
  for (std::size_t i = 0; i < count; ++i) {
    const auto started = clock_type::now();
    auto found = index.find(liquid_digest(queries[i].key));
    if (!found) co_return std::unexpected(found.error());
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - started);
    latencies.push_back(static_cast<std::uint64_t>(elapsed.count()));
    if (found->has_value() != queries[i].expect_hit)
      co_return std::unexpected(liquid::error{liquid::errc::corrupt, "liquid lookup returned wrong presence", {}});
  }
  co_return finish_phase(std::move(name), count, phase_started, latencies);
}

liquid::runtime::task<phase_result>
run_liquid_ranges(liquid::store_detail::chunk_index &index,
                  const std::vector<key_type> &keys) {
  std::vector<std::uint64_t> latencies;
  latencies.reserve(range_scan_count);
  std::mt19937_64 random(3);
  const auto phase_started = clock_type::now();
  for (std::size_t i = 0; i < range_scan_count; ++i) {
    const auto started = clock_type::now();
    for (std::size_t n = 0; n < range_scan_width; ++n) {
      const auto &key = keys[static_cast<std::size_t>((random() + n) % keys.size())];
      auto found = index.find(liquid_digest(key));
      if (!found) co_return std::unexpected(found.error());
      if (!*found) co_return std::unexpected(liquid::error{liquid::errc::corrupt, "liquid range lookup missed", {}});
    }
    const auto elapsed = std::chrono::duration_cast<std::chrono::nanoseconds>(clock_type::now() - started);
    latencies.push_back(static_cast<std::uint64_t>(elapsed.count()));
  }
  co_return finish_phase("range_scan_64", range_scan_count, phase_started, latencies);
}

template <class T> T checked(liquid::result<T> value, std::string_view operation) {
  if (!value) fail(std::format("liquid {} failed: {}", operation, value.error().message));
  return std::move(*value);
}

void checked(liquid::result<void> value, std::string_view operation) {
  if (!value) fail(std::format("liquid {} failed: {}", operation, value.error().message));
}

int run_liquid(const config &settings, const std::vector<key_type> &keys,
               const std::vector<query> &queries) {
  std::cout << std::format("config engine=liquid n={} seed={} batch_size={} point_lookups={} range_scans={} range_width={} cold_lookups={} tuning={} record_bytes=48 memtable_entries=1048576 bloom_bits_per_key=10 compaction_k=4\n",
                           settings.n, seed, batch_size, point_lookup_count, range_scan_count,
                           range_scan_width, cold_lookup_count, settings.tuning);
  liquid::store_detail::chunk_index index(settings.directory);
  checked(liquid::runtime::block_on(index.open()), "open");
  liquid::store_detail::reset_file_write_counter();
  print_phase(checked(liquid::runtime::block_on(run_liquid_inserts(index, keys)), "insert"));
  print_phase(checked(liquid::runtime::block_on(run_liquid_points(index, queries, point_lookup_count, "point_lookup")), "point lookup"));
  print_phase(checked(liquid::runtime::block_on(run_liquid_ranges(index, keys)), "range scan"));
  checked(liquid::runtime::block_on(index.flush()), "flush");
  checked(liquid::runtime::block_on(index.close()), "close");
  const auto cache_status = drop_page_cache(settings.directory);
  liquid::store_detail::chunk_index cold(settings.directory);
  checked(liquid::runtime::block_on(cold.open()), "cold open");
  print_phase(checked(liquid::runtime::block_on(run_liquid_points(cold, queries, cold_lookup_count, "cold_point_lookup")), "cold point lookup"));
  checked(liquid::runtime::block_on(cold.close()), "cold close");
  const auto on_disk = directory_size(settings.directory);
  const auto written = liquid::store_detail::file_write_counter();
  const long double logical_bytes = static_cast<long double>(settings.n) * 48.0L;
  std::cout << std::format("storage on_disk_bytes={} bytes_written={} write_amplification={:.6Lf} peak_rss_bytes={} cache_drop={}\n",
                           on_disk, written, static_cast<long double>(written) / logical_bytes,
                           peak_rss_bytes(), cache_status);
  return 0;
}

void print_phase(const phase_result &phase) {
  std::cout << std::format("phase={} operations={} seconds={:.6f} "
                           "ops_per_sec={:.2f} p50_us={:.3f} p99_us={:.3f}\n",
                           phase.name, phase.operations, phase.seconds,
                           phase.operations / phase.seconds, phase.p50_us,
                           phase.p99_us);
}

int run(const config &settings) {
  prepare_directory(settings.directory);
  const auto keys = generate_keys(settings.n);
  const auto queries = generate_queries(keys, point_lookup_count);

  if (settings.engine == "liquid") return run_liquid(settings, keys, queries);

  std::unique_ptr<engine> database;
  if (settings.engine == "rocksdb") {
    database = std::make_unique<rocks_engine>(settings.directory,
                                              settings.tuning == "tuned");
  } else if (settings.engine == "lmdb") {
    database = std::make_unique<lmdb_engine>(settings.directory,
                                             lmdb_map_size(settings),
                                             settings.tuning == "nosync");
  }

  std::cout << std::format("config engine={} n={} seed={} batch_size={} "
                           "point_lookups={} range_scans={} "
                           "range_width={} cold_lookups={} tuning={} {}\n",
                           settings.engine, settings.n, seed, batch_size,
                           point_lookup_count, range_scan_count,
                           range_scan_width, cold_lookup_count, settings.tuning,
                           database->details());

  database->open(true);
  print_phase(run_inserts(*database, keys));
  print_phase(
      run_points(*database, queries, point_lookup_count, "point_lookup"));
  print_phase(run_ranges(*database, keys));
  database->flush();
  database->close();

  const std::string cache_status = drop_page_cache(settings.directory);
  database->open(false);
  print_phase(
      run_points(*database, queries, cold_lookup_count, "cold_point_lookup"));
  database->close();

  const std::uint64_t on_disk = directory_size(settings.directory);
  std::uint64_t written = database->bytes_written();
  if (settings.engine == "lmdb") {
    written = on_disk;
  }
  const long double logical_bytes =
      static_cast<long double>(settings.n) * 48.0L;
  std::cout << std::format(
      "storage on_disk_bytes={} bytes_written={} write_amplification={:.6Lf} "
      "peak_rss_bytes={} cache_drop={}\n",
      on_disk, written, static_cast<long double>(written) / logical_bytes,
      peak_rss_bytes(), cache_status);
  return 0;
}

} // namespace

int main(int argc, char **argv) {
  try {
    return run(parse_arguments(argc, argv));
  } catch (const std::exception &error) {
    std::cerr << "chunk-index-bench: " << error.what() << '\n';
    return 1;
  }
}
