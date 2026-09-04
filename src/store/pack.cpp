#include "pack.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <limits>
#include <system_error>

#include <boost/capy/buffers.hpp>
#include <boost/capy/ex/this_coro.hpp>
#include <zstd.h>

#include "../runtime/net.hpp"

namespace liquid::store_detail {
namespace {

std::atomic<std::uint64_t> file_read_bytes{};
std::atomic<std::uint64_t> file_written_bytes{};

constexpr std::array<std::byte, 8> pack_magic{std::byte{'L'}, std::byte{'Q'}, std::byte{'P'},
                                              std::byte{'A'}, std::byte{'C'}, std::byte{'K'},
                                              std::byte{1},   std::byte{0}};
constexpr std::array<std::byte, 8> footer_magic{std::byte{'L'}, std::byte{'Q'}, std::byte{'F'},
                                                std::byte{'O'}, std::byte{'O'}, std::byte{'T'},
                                                std::byte{1},   std::byte{0}};
constexpr std::size_t pack_header_size = 16;
constexpr std::size_t physical_header_size = 49;
constexpr std::size_t table_entry_size = 53;
constexpr std::size_t footer_size = 32;

error io_error(std::string message, std::string context) {
  return {errc::io, std::move(message), std::move(context)};
}
error corrupt_error(std::string message, const std::filesystem::path &path) {
  return {errc::corrupt, std::move(message), path.string()};
}

void put_u32(std::vector<std::byte> &out, std::uint32_t value) {
  for (unsigned i = 0; i != 4; ++i)
    out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
}
void put_u64(std::vector<std::byte> &out, std::uint64_t value) {
  for (unsigned i = 0; i != 8; ++i)
    out.push_back(std::byte{static_cast<unsigned char>(value >> (i * 8U))});
}
std::uint32_t get_u32(std::span<const std::byte> in, std::size_t at) {
  if (at + 4 > in.size())
    return 0;
  std::uint32_t value = 0;
  for (unsigned i = 0; i != 4; ++i)
    value |= std::uint32_t(std::to_integer<unsigned char>(in[at + i])) << (i * 8U);
  return value;
}
std::uint64_t get_u64(std::span<const std::byte> in, std::size_t at) {
  if (at + 8 > in.size())
    return 0;
  std::uint64_t value = 0;
  for (unsigned i = 0; i != 8; ++i)
    value |= std::uint64_t(std::to_integer<unsigned char>(in[at + i])) << (i * 8U);
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
         value == static_cast<unsigned char>(algorithm::sha256);
}

runtime::task<void> write_at(std::filesystem::path path, std::uint64_t offset,
                             std::vector<std::byte> bytes, bool truncate, bool sync = true) {
  try {
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::corosio::random_access_file file(executor.context());
    auto flags = boost::corosio::file_base::read_write | boost::corosio::file_base::create;
    if (truncate)
      flags |= boost::corosio::file_base::truncate;
    file.open(path, flags);
    std::size_t written = 0;
    while (written < bytes.size()) {
      auto [ec, count] = co_await file.write_some_at(
          offset + written,
          boost::capy::const_buffer(bytes.data() + written, bytes.size() - written));
      if (ec || count == 0)
        co_return std::unexpected(io_error(ec ? ec.message() : "short file write", path.string()));
      written += count;
      file_written_bytes.fetch_add(count, std::memory_order_relaxed);
    }
    if (sync)
      file.sync_data();
    file.close();
    co_return result<void>{};
  } catch (const std::system_error &exception) {
    co_return std::unexpected(io_error(exception.what(), path.string()));
  } catch (const std::exception &exception) {
    co_return std::unexpected(error{errc::internal, exception.what(), path.string()});
  }
}

runtime::task<void> resize_file(std::filesystem::path path, std::uint64_t size) {
  try {
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::corosio::random_access_file file(executor.context());
    file.open(path, boost::corosio::file_base::read_write);
    file.resize(size);
    file.sync_data();
    file.close();
    co_return result<void>{};
  } catch (const std::system_error &exception) {
    co_return std::unexpected(io_error(exception.what(), path.string()));
  }
}

std::vector<std::byte> encode_table(const pack_contents &contents) {
  std::vector<pack_entry> sorted = contents.entries;
  std::sort(sorted.begin(), sorted.end(), [](const pack_entry &left, const pack_entry &right) {
    if (left.id.bytes != right.id.bytes)
      return left.id.bytes < right.id.bytes;
    return static_cast<unsigned char>(left.id.alg) < static_cast<unsigned char>(right.id.alg);
  });
  std::vector<std::byte> table;
  table.reserve(sorted.size() * table_entry_size);
  for (const auto &entry : sorted) {
    table.push_back(std::byte{static_cast<unsigned char>(entry.id.alg)});
    table.insert(table.end(), entry.id.bytes.begin(), entry.id.bytes.end());
    put_u64(table, entry.location.offset);
    put_u32(table, entry.location.length);
    put_u32(table, entry.raw_length);
    put_u32(table, entry.location.pack_id);
  }
  return table;
}

std::vector<std::byte> encode_footer(std::uint64_t table_offset, std::uint64_t table_size,
                                     std::span<const std::byte> table) {
  std::vector<std::byte> footer;
  footer.insert(footer.end(), footer_magic.begin(), footer_magic.end());
  put_u64(footer, table_offset);
  put_u64(footer, table_size);
  put_u64(footer, checksum(table));
  return footer;
}

} // namespace

runtime::task<std::vector<std::byte>> read_file(std::filesystem::path path) {
  try {
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::corosio::random_access_file file(executor.context());
    file.open(path, boost::corosio::file_base::read_only);
    const auto size = file.size();
    if (size > std::numeric_limits<std::size_t>::max()) {
      co_return std::unexpected(io_error("file is too large to address", path.string()));
    }
    std::vector<std::byte> bytes(static_cast<std::size_t>(size));
    std::size_t read = 0;
    while (read < bytes.size()) {
      auto [ec, count] = co_await file.read_some_at(
          read, boost::capy::mutable_buffer(bytes.data() + read, bytes.size() - read));
      if (ec || count == 0)
        co_return std::unexpected(
            io_error(ec ? ec.message() : "unexpected end of file", path.string()));
      read += count;
      note_file_read(count);
    }
    file.close();
    co_return bytes;
  } catch (const std::system_error &exception) {
    co_return std::unexpected(io_error(exception.what(), path.string()));
  } catch (const std::exception &exception) {
    co_return std::unexpected(error{errc::internal, exception.what(), path.string()});
  }
}

runtime::task<std::vector<std::byte>> read_file_range(std::filesystem::path path,
                                                      std::uint64_t offset, std::size_t size) {
  try {
    const auto executor = co_await boost::capy::this_coro::executor;
    boost::corosio::random_access_file file(executor.context());
    file.open(path, boost::corosio::file_base::read_only);
    if (offset > file.size() || size > file.size() - offset) {
      co_return std::unexpected(io_error("requested range is outside file", path.string()));
    }
    std::vector<std::byte> bytes(size);
    std::size_t read = 0;
    while (read < bytes.size()) {
      auto [ec, count] = co_await file.read_some_at(
          offset + read, boost::capy::mutable_buffer(bytes.data() + read, bytes.size() - read));
      if (ec || count == 0)
        co_return std::unexpected(
            io_error(ec ? ec.message() : "unexpected end of file", path.string()));
      read += count;
      note_file_read(count);
    }
    file.close();
    co_return bytes;
  } catch (const std::system_error &exception) {
    co_return std::unexpected(io_error(exception.what(), path.string()));
  } catch (const std::exception &exception) {
    co_return std::unexpected(error{errc::internal, exception.what(), path.string()});
  }
}

runtime::task<void> write_file(std::filesystem::path path, std::vector<std::byte> bytes) {
  co_return co_await write_at(std::move(path), 0, std::move(bytes), true);
}

runtime::task<void> write_file_range(std::filesystem::path path, std::uint64_t offset,
                                     std::vector<std::byte> bytes) {
  co_return co_await write_at(std::move(path), offset, std::move(bytes), false, false);
}

void reset_file_read_counter() noexcept { file_read_bytes.store(0, std::memory_order_relaxed); }
std::uint64_t file_read_counter() noexcept {
  return file_read_bytes.load(std::memory_order_relaxed);
}
void note_file_read(std::size_t bytes) noexcept {
  file_read_bytes.fetch_add(bytes, std::memory_order_relaxed);
}
void reset_file_write_counter() noexcept { file_written_bytes.store(0, std::memory_order_relaxed); }
std::uint64_t file_write_counter() noexcept {
  return file_written_bytes.load(std::memory_order_relaxed);
}

runtime::task<pack_contents> read_pack(std::filesystem::path path, std::uint32_t id) {
  auto file = co_await read_file(path);
  if (!file)
    co_return std::unexpected(file.error());
  const auto bytes = std::span<const std::byte>(*file);
  if (bytes.size() < pack_header_size ||
      !std::equal(pack_magic.begin(), pack_magic.end(), bytes.begin())) {
    co_return std::unexpected(corrupt_error("invalid pack header", path));
  }
  if (get_u32(bytes, 8) != id)
    co_return std::unexpected(corrupt_error("pack id does not match filename", path));
  pack_contents result{id, {}, pack_header_size};
  if (bytes.size() == pack_header_size)
    co_return result;
  if (bytes.size() < footer_size ||
      !std::equal(footer_magic.begin(), footer_magic.end(), bytes.end() - footer_size)) {
    co_return std::unexpected(corrupt_error("missing or invalid pack footer", path));
  }
  const auto footer_at = bytes.size() - footer_size;
  const auto table_at = get_u64(bytes, footer_at + 8);
  const auto table_size = get_u64(bytes, footer_at + 16);
  const auto table_hash = get_u64(bytes, footer_at + 24);
  if (table_at < pack_header_size || table_at + table_size != footer_at ||
      table_size % table_entry_size != 0 ||
      checksum(bytes.subspan(table_at, table_size)) != table_hash) {
    co_return std::unexpected(corrupt_error("invalid pack table", path));
  }
  result.data_end = table_at;
  for (std::size_t at = static_cast<std::size_t>(table_at); at < footer_at;
       at += table_entry_size) {
    const auto alg = std::to_integer<unsigned char>(bytes[at]);
    if (!valid_algorithm(alg))
      co_return std::unexpected(corrupt_error("unknown digest algorithm in pack table", path));
    pack_entry entry{};
    entry.id.alg = static_cast<algorithm>(alg);
    std::copy_n(bytes.begin() + at + 1, entry.id.bytes.size(), entry.id.bytes.begin());
    entry.location = {get_u32(bytes, at + 49), get_u64(bytes, at + 33), get_u32(bytes, at + 41)};
    entry.raw_length = get_u32(bytes, at + 45);
    if (entry.location.offset < pack_header_size ||
        entry.location.offset + physical_header_size + entry.location.length > table_at) {
      co_return std::unexpected(corrupt_error("pack table location is outside data", path));
    }
    result.entries.push_back(entry);
  }
  co_return result;
}

runtime::task<pack_entry> append_pack_entry(std::filesystem::path path, pack_contents contents,
                                            digest id, std::vector<std::byte> raw) {
  if (contents.data_end < pack_header_size) {
    co_return std::unexpected(error{errc::internal, "invalid pack write position", path.string()});
  }
  if (std::filesystem::exists(path)) {
    auto truncation = co_await resize_file(path, contents.data_end);
    if (!truncation)
      co_return std::unexpected(truncation.error());
  } else {
    std::vector<std::byte> header(pack_magic.begin(), pack_magic.end());
    put_u32(header, contents.id);
    put_u32(header, 0);
    auto created = co_await write_file(path, std::move(header));
    if (!created)
      co_return std::unexpected(created.error());
  }
  const auto bound = ZSTD_compressBound(raw.size());
  std::vector<std::byte> compressed(bound);
  const auto compressed_size =
      ZSTD_compress(compressed.data(), compressed.size(), raw.data(), raw.size(), 3);
  if (ZSTD_isError(compressed_size) ||
      compressed_size > std::numeric_limits<std::uint32_t>::max() ||
      raw.size() > std::numeric_limits<std::uint32_t>::max()) {
    co_return std::unexpected(error{errc::internal, "zstd compression failed", path.string()});
  }
  compressed.resize(compressed_size);
  pack_entry entry{id,
                   {contents.id, contents.data_end, static_cast<std::uint32_t>(compressed.size())},
                   static_cast<std::uint32_t>(raw.size())};
  std::vector<std::byte> physical;
  physical.reserve(physical_header_size + compressed.size());
  physical.push_back(std::byte{static_cast<unsigned char>(id.alg)});
  physical.insert(physical.end(), id.bytes.begin(), id.bytes.end());
  put_u32(physical, entry.raw_length);
  put_u32(physical, entry.location.length);
  put_u64(physical, 0);
  physical.insert(physical.end(), compressed.begin(), compressed.end());
  auto data_written = co_await write_at(path, contents.data_end, std::move(physical), false);
  if (!data_written)
    co_return std::unexpected(data_written.error());
  contents.entries.push_back(entry);
  contents.data_end += physical_header_size + entry.location.length;
  const auto table = encode_table(contents);
  auto table_written = co_await write_at(path, contents.data_end, table, false);
  if (!table_written)
    co_return std::unexpected(table_written.error());
  auto footer_written =
      co_await write_at(path, contents.data_end + table.size(),
                        encode_footer(contents.data_end, table.size(), table), false);
  if (!footer_written)
    co_return std::unexpected(footer_written.error());
  co_return entry;
}

runtime::task<std::vector<std::byte>> read_pack_entry(std::filesystem::path path,
                                                      chunk_location location, digest expected) {
  auto header = co_await read_file_range(path, location.offset, physical_header_size);
  if (!header)
    co_return std::unexpected(header.error());
  const auto bytes = std::span<const std::byte>(*header);
  if (std::to_integer<unsigned char>(bytes[0]) != static_cast<unsigned char>(expected.alg) ||
      !std::equal(expected.bytes.begin(), expected.bytes.end(), bytes.begin() + 1)) {
    co_return std::unexpected(corrupt_error("chunk header digest differs from index", path));
  }
  const auto raw_size = get_u32(bytes, 33);
  const auto compressed_size = get_u32(bytes, 37);
  if (compressed_size != location.length)
    co_return std::unexpected(corrupt_error("chunk length differs from index", path));
  auto compressed =
      co_await read_file_range(path, location.offset + physical_header_size, compressed_size);
  if (!compressed)
    co_return std::unexpected(compressed.error());
  std::vector<std::byte> raw(raw_size);
  const auto produced =
      ZSTD_decompress(raw.data(), raw.size(), compressed->data(), compressed_size);
  if (ZSTD_isError(produced) || produced != raw.size())
    co_return std::unexpected(corrupt_error("chunk decompression failed", path));
  const auto actual = hash(expected.alg, raw);
  if (!actual || actual->bytes != expected.bytes) {
    co_return std::unexpected(
        error{errc::corrupt, "chunk digest verification failed", to_string(expected)});
  }
  co_return raw;
}

} // namespace liquid::store_detail
