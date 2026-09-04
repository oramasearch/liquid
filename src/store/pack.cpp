#include "store/pack.hpp"

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <fstream>
#include <limits>
#include <string>
#include <system_error>

#define ZSTD_STATIC_LINKING_ONLY
#include <zstd.h>

namespace liquid::store_detail {
namespace {

constexpr std::array<std::byte, 4> record_magic{
    std::byte{0x4c}, std::byte{0x43}, std::byte{0x48}, std::byte{0x4b}};
constexpr std::array<char, 8> table_magic{'L', 'Q', 'T', 'A', 'B', 'L', 'E', '1'};

template <class Integer>
void append_little_endian(std::vector<std::byte>& output, Integer value) {
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        output.push_back(std::byte{static_cast<unsigned char>((value >> (index * 8U)) & 0xffU)});
    }
}

template <class Integer>
Integer read_little_endian(std::span<const std::byte> input, std::size_t offset) {
    Integer value = 0;
    for (std::size_t index = 0; index < sizeof(Integer); ++index) {
        value |= static_cast<Integer>(std::to_integer<unsigned char>(input[offset + index]))
                 << (index * 8U);
    }
    return value;
}

bool same_digest(const digest& left, const digest& right) noexcept {
    return left.alg == right.alg && left.bytes == right.bytes;
}

error corrupt(const digest& id, std::string message) {
    return error{errc::corrupt, std::move(message), to_string(id)};
}

result<std::vector<std::byte>> compress(std::span<const std::byte> input) {
    std::vector<std::byte> output(ZSTD_compressBound(input.size()));
    ZSTD_CCtx* raw = ZSTD_createCCtx();
    if (!raw) return std::unexpected(error{errc::internal, "could not allocate zstd context", "pack"});
    const auto cleanup = [&] { ZSTD_freeCCtx(raw); };

    auto status = ZSTD_CCtx_setParameter(raw, ZSTD_c_compressionLevel, 3);
    if (!ZSTD_isError(status)) {
        status = ZSTD_CCtx_setParameter(raw, ZSTD_c_enableLongDistanceMatching, 1);
    }
    if (!ZSTD_isError(status)) {
        status = ZSTD_CCtx_setParameter(raw, ZSTD_c_nbWorkers, 1);
    }
    if (!ZSTD_isError(status)) {
        status = ZSTD_CCtx_setParameter(raw, ZSTD_c_rsyncable, 1);
    }
    if (ZSTD_isError(status)) {
        const std::string message = ZSTD_getErrorName(status);
        cleanup();
        return std::unexpected(error{errc::internal, message, "zstd parameters"});
    }
    status = ZSTD_compress2(raw, output.data(), output.size(), input.data(), input.size());
    if (ZSTD_isError(status)) {
        const std::string message = ZSTD_getErrorName(status);
        cleanup();
        return std::unexpected(error{errc::internal, message, "zstd compression"});
    }
    cleanup();
    output.resize(status);
    return output;
}

}  // namespace

pack_store::pack_store(
    std::filesystem::path directory, std::uint64_t target_size, std::uint32_t pack_id)
    : directory_(std::move(directory)), target_size_(target_size), pack_id_(pack_id) {}

result<pack_store> pack_store::open(
    const std::filesystem::path& directory, std::uint64_t target_size) {
    std::error_code filesystem_error;
    std::filesystem::create_directories(directory, filesystem_error);
    if (filesystem_error) {
        return std::unexpected(error{errc::io, filesystem_error.message(), directory.string()});
    }

    std::uint32_t latest = 0;
    bool found = false;
    for (const auto& entry : std::filesystem::directory_iterator(directory)) {
        if (!entry.is_regular_file() || entry.path().extension() != ".pack") continue;
        const auto stem = entry.path().stem().string();
        std::uint32_t value = 0;
        const auto parsed = std::from_chars(stem.data(), stem.data() + stem.size(), value);
        if (parsed.ec == std::errc{} && parsed.ptr == stem.data() + stem.size()) {
            if (!found || value > latest) latest = value;
            found = true;
        }
    }

    pack_store output(directory, target_size, found ? latest : 0);
    auto table = output.ensure_table(output.pack_id_);
    if (!table) return std::unexpected(table.error());
    return output;
}

std::filesystem::path pack_store::pack_path(std::uint32_t pack_id) const {
    return directory_ / (std::to_string(pack_id) + ".pack");
}

std::filesystem::path pack_store::table_path(std::uint32_t pack_id) const {
    return directory_ / (std::to_string(pack_id) + ".table");
}

result<void> pack_store::ensure_table(std::uint32_t pack_id) const {
    const auto path = table_path(pack_id);
    std::error_code filesystem_error;
    if (std::filesystem::exists(path, filesystem_error)) return {};
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    if (!output) return std::unexpected(error{errc::io, "could not create pack table", path.string()});
    output.write(table_magic.data(), static_cast<std::streamsize>(table_magic.size()));
    output.flush();
    if (!output) return std::unexpected(error{errc::io, "could not write pack table", path.string()});
    return {};
}

result<chunk_location> pack_store::append(
    const digest& id, std::span<const std::byte> bytes) {
    if (bytes.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error{errc::invalid_argument, "chunk is too large", to_string(id)});
    }
    auto compressed = compress(bytes);
    if (!compressed) return std::unexpected(compressed.error());
    const auto record_size = pack_record_header_size + compressed->size();
    if (record_size > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(error{errc::invalid_argument, "pack record is too large", to_string(id)});
    }

    std::error_code filesystem_error;
    auto current_path = pack_path(pack_id_);
    auto current_size = std::filesystem::exists(current_path, filesystem_error)
                            ? std::filesystem::file_size(current_path, filesystem_error)
                            : 0;
    if (filesystem_error) {
        return std::unexpected(error{errc::io, filesystem_error.message(), current_path.string()});
    }
    if (current_size != 0 && current_size + record_size > target_size_) {
        if (pack_id_ == std::numeric_limits<std::uint32_t>::max()) {
            return std::unexpected(error{errc::io, "pack id space exhausted", directory_.string()});
        }
        ++pack_id_;
        current_path = pack_path(pack_id_);
        current_size = 0;
        auto table = ensure_table(pack_id_);
        if (!table) return std::unexpected(table.error());
    }

    std::vector<std::byte> header;
    header.reserve(pack_record_header_size);
    header.insert(header.end(), record_magic.begin(), record_magic.end());
    header.push_back(std::byte{1});
    header.push_back(std::byte{static_cast<unsigned char>(id.alg)});
    header.push_back(std::byte{0});
    header.push_back(std::byte{0});
    append_little_endian(header, static_cast<std::uint32_t>(bytes.size()));
    append_little_endian(header, static_cast<std::uint32_t>(compressed->size()));
    header.insert(header.end(), id.bytes.begin(), id.bytes.end());

    std::ofstream pack(current_path, std::ios::binary | std::ios::app);
    if (!pack) return std::unexpected(error{errc::io, "could not open pack", current_path.string()});
    pack.write(reinterpret_cast<const char*>(header.data()), static_cast<std::streamsize>(header.size()));
    pack.write(
        reinterpret_cast<const char*>(compressed->data()),
        static_cast<std::streamsize>(compressed->size()));
    pack.flush();
    if (!pack) return std::unexpected(error{errc::io, "could not append pack record", current_path.string()});

    const chunk_location location{
        pack_id_, current_size, static_cast<std::uint32_t>(record_size)};
    std::vector<std::byte> table_entry;
    table_entry.reserve(49);
    table_entry.push_back(std::byte{static_cast<unsigned char>(id.alg)});
    table_entry.insert(table_entry.end(), id.bytes.begin(), id.bytes.end());
    append_little_endian(table_entry, location.offset);
    append_little_endian(table_entry, location.length);
    append_little_endian(table_entry, static_cast<std::uint32_t>(bytes.size()));

    const auto sidecar_path = table_path(pack_id_);
    std::ofstream sidecar(sidecar_path, std::ios::binary | std::ios::app);
    if (!sidecar) {
        return std::unexpected(error{errc::io, "could not open pack table", sidecar_path.string()});
    }
    sidecar.write(
        reinterpret_cast<const char*>(table_entry.data()),
        static_cast<std::streamsize>(table_entry.size()));
    sidecar.flush();
    if (!sidecar) {
        return std::unexpected(error{errc::io, "could not append pack table", sidecar_path.string()});
    }
    return location;
}

result<std::vector<std::byte>> pack_store::read(
    const digest& expected, const chunk_location& location) const {
    if (location.length < pack_record_header_size) {
        return std::unexpected(corrupt(expected, "pack record is shorter than its header"));
    }
    const auto path = pack_path(location.pack_id);
    std::ifstream input(path, std::ios::binary);
    if (!input) return std::unexpected(error{errc::io, "could not open pack", path.string()});
    input.seekg(static_cast<std::streamoff>(location.offset));
    std::vector<std::byte> record(location.length);
    input.read(reinterpret_cast<char*>(record.data()), static_cast<std::streamsize>(record.size()));
    if (input.gcount() != static_cast<std::streamsize>(record.size())) {
        return std::unexpected(corrupt(expected, "pack record is truncated"));
    }

    if (!std::equal(record_magic.begin(), record_magic.end(), record.begin()) ||
        record[4] != std::byte{1}) {
        return std::unexpected(corrupt(expected, "pack record header is invalid"));
    }
    digest recorded{static_cast<algorithm>(std::to_integer<unsigned char>(record[5])), {}};
    std::copy_n(record.begin() + 16, recorded.bytes.size(), recorded.bytes.begin());
    if (!same_digest(recorded, expected)) {
        return std::unexpected(corrupt(expected, "pack record digest does not match the index"));
    }

    const auto raw_size = read_little_endian<std::uint32_t>(record, 8);
    const auto compressed_size = read_little_endian<std::uint32_t>(record, 12);
    if (compressed_size != record.size() - pack_record_header_size) {
        return std::unexpected(corrupt(expected, "pack record length is invalid"));
    }
    std::vector<std::byte> output(raw_size);
    const auto status = ZSTD_decompress(
        output.data(), output.size(), record.data() + pack_record_header_size, compressed_size);
    if (ZSTD_isError(status) || status != output.size()) {
        return std::unexpected(corrupt(expected, "zstd chunk decompression failed"));
    }
    const auto actual = hash(expected.alg, output);
    if (!actual || !same_digest(*actual, expected)) {
        return std::unexpected(corrupt(expected, "chunk digest verification failed"));
    }
    return output;
}

}  // namespace liquid::store_detail
