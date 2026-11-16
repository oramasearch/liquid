// SPDX-License-Identifier: MIT

#include "liquid/identity.hpp"

#include <algorithm>
#include <array>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <format>
#include <fstream>
#include <limits>
#include <map>
#include <optional>
#include <random>
#include <set>
#include <sstream>
#include <utility>
#include <variant>

#if defined(_WIN32)
#include <windows.h>
#else
#include <cerrno>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include "crypto/crypto.hpp"
#include "crypto/signature.hpp"
#include "encoding/cbor.hpp"
#include "identity/custody/common.hpp"
#include "identity/custody/providers.hpp"
#include "identity/key_version.hpp"
#include "identity/keys/backend.hpp"
#include "identity/taap.hpp"
#include "monocypher-ed25519.h"
#include "monocypher.h"
#include "store/pack.hpp"

namespace liquid {
namespace {

constexpr std::array<std::byte, 8> key_magic_v1{std::byte{'L'},
                                                std::byte{'Q'},
                                                std::byte{'K'},
                                                std::byte{'E'},
                                                std::byte{'Y'},
                                                std::byte{1},
                                                std::byte{0},
                                                std::byte{0}};
constexpr std::array<std::byte, 8> key_magic_v2{std::byte{'L'},
                                                std::byte{'Q'},
                                                std::byte{'K'},
                                                std::byte{'E'},
                                                std::byte{'Y'},
                                                std::byte{2},
                                                std::byte{0},
                                                std::byte{0}};
constexpr std::array<std::byte, 8> key_magic_v3{std::byte{'L'},
                                                std::byte{'Q'},
                                                std::byte{'K'},
                                                std::byte{'E'},
                                                std::byte{'Y'},
                                                std::byte{3},
                                                std::byte{0},
                                                std::byte{0}};
constexpr std::string_view key_wrap_context = "liquid store data key wrapping v1";
constexpr std::size_t boxed_key_size = 24 + 16 + 32;

error key_error(errc code, std::string message, std::string context) {
    return {code, std::move(message), std::move(context)};
}

void put_u32(std::vector<std::byte>& out, std::uint32_t value) {
    for (unsigned index = 0; index != 4; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

void put_u16(std::vector<std::byte>& out, std::uint16_t value) {
    for (unsigned index = 0; index != 2; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

void put_u64(std::vector<std::byte>& out, std::uint64_t value) {
    for (unsigned index = 0; index != 8; ++index)
        out.push_back(std::byte{static_cast<unsigned char>(value >> (index * 8U))});
}

std::uint32_t get_u32(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 4 > bytes.size())
        return 0;
    std::uint32_t value = 0;
    for (unsigned index = 0; index != 4; ++index)
        value |= std::uint32_t(std::to_integer<unsigned char>(bytes[at + index])) << (index * 8U);
    return value;
}

std::uint16_t get_u16(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 2 > bytes.size())
        return 0;
    std::uint16_t value = 0;
    for (unsigned index = 0; index != 2; ++index)
        value |= std::uint16_t(std::to_integer<unsigned char>(bytes[at + index])) << (index * 8U);
    return value;
}

std::uint64_t get_u64(std::span<const std::byte> bytes, std::size_t at) {
    if (at + 8 > bytes.size())
        return 0;
    std::uint64_t value = 0;
    for (unsigned index = 0; index != 8; ++index)
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[at + index])) << (index * 8U);
    return value;
}

bool valid_key_id(std::string_view key_id) {
    return !key_id.empty() && key_id != "." && key_id != ".." &&
           key_id.size() <= std::numeric_limits<std::uint16_t>::max() &&
           key_id.find_first_of("/\\@") == std::string_view::npos &&
           key_id.find('\0') == std::string_view::npos;
}

result<void> validate_argon2(argon2_parameters parameters) {
    if (parameters.lanes == 0 || parameters.iterations == 0 ||
        parameters.memory_kib < 8U * parameters.lanes) {
        return std::unexpected(
            key_error(errc::invalid_argument, "invalid Argon2i parameters", "passphrase_provider"));
    }
    return {};
}

struct wrapped_key_entry {
    std::string custody;
    std::vector<std::byte> parameters;
    std::vector<std::byte> wrapped;
    bool legacy{};
};

struct wrapped_key_file {
    std::uint8_t format{3};
    std::array<std::byte, 16> salt{};
    argon2_parameters parameters{};
    std::uint64_t current_version{1};
    std::map<std::uint64_t, wrapped_key_entry> versions;
    std::set<std::uint64_t> revoked;
};

result<wrapped_key_file> parse_wrapped_key_file(std::span<const std::byte> bytes,
                                                std::string_view key_id) {
    constexpr std::size_t legacy_header_size = key_magic_v1.size() + 16 + 12;
    const bool legacy = bytes.size() == legacy_header_size + boxed_key_size &&
                        std::equal(key_magic_v1.begin(), key_magic_v1.end(), bytes.begin());
    const bool version_two =
        bytes.size() >= 52 && std::equal(key_magic_v2.begin(), key_magic_v2.end(), bytes.begin());
    const bool version_three =
        bytes.size() >= 24 && std::equal(key_magic_v3.begin(), key_magic_v3.end(), bytes.begin());
    if (!legacy && !version_two && !version_three) {
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped data key", std::string(key_id)));
    }
    wrapped_key_file output;
    if (version_three) {
        output.format = 3;
        output.current_version = get_u64(bytes, 8);
        const auto version_count = get_u32(bytes, 16);
        const auto revoked_count = get_u32(bytes, 20);
        auto at = std::size_t{24};
        for (auto index = std::uint32_t{}; index != version_count; ++index) {
            if (bytes.size() - at < 18) {
                return std::unexpected(key_error(
                    errc::corrupt, "truncated wrapped key-version entry", std::string(key_id)));
            }
            const auto version = get_u64(bytes, at);
            const auto custody_size = get_u16(bytes, at + 8);
            const auto parameters_size = get_u32(bytes, at + 10);
            const auto wrapped_size = get_u32(bytes, at + 14);
            at += 18;
            const auto payload_size =
                static_cast<std::size_t>(custody_size) + parameters_size + wrapped_size;
            if (version == 0 || custody_size == 0 || payload_size > bytes.size() - at ||
                output.versions.contains(version)) {
                return std::unexpected(key_error(
                    errc::corrupt, "invalid wrapped key-version entry", std::string(key_id)));
            }
            auto entry = wrapped_key_entry{};
            entry.custody =
                std::string{reinterpret_cast<const char*>(bytes.data() + at), custody_size};
            at += custody_size;
            entry.parameters.assign(bytes.begin() + at, bytes.begin() + at + parameters_size);
            at += parameters_size;
            entry.wrapped.assign(bytes.begin() + at, bytes.begin() + at + wrapped_size);
            at += wrapped_size;
            output.versions.emplace(version, std::move(entry));
        }
        for (auto index = std::uint32_t{}; index != revoked_count; ++index) {
            if (bytes.size() - at < 8) {
                return std::unexpected(
                    key_error(errc::corrupt, "truncated revoked key version", std::string(key_id)));
            }
            const auto version = get_u64(bytes, at);
            at += 8;
            if (version == 0 || output.versions.contains(version) ||
                !output.revoked.insert(version).second) {
                return std::unexpected(
                    key_error(errc::corrupt, "invalid revoked key version", std::string(key_id)));
            }
        }
        if (at != bytes.size() || output.current_version == 0 ||
            (!output.versions.contains(output.current_version) &&
             !output.revoked.contains(output.current_version))) {
            return std::unexpected(
                key_error(errc::corrupt, "invalid wrapped key-version table", std::string(key_id)));
        }
        return output;
    }
    output.format = legacy ? 1 : 2;
    std::copy_n(bytes.begin() + key_magic_v1.size(), output.salt.size(), output.salt.begin());
    output.parameters = {get_u32(bytes, 24), get_u32(bytes, 28), get_u32(bytes, 32)};
    auto valid = validate_argon2(output.parameters);
    if (!valid)
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped data-key parameters", std::string(key_id)));
    if (legacy) {
        output.versions.emplace(
            1,
            wrapped_key_entry{.wrapped = {bytes.begin() + legacy_header_size, bytes.end()},
                              .legacy = true});
        return output;
    }
    output.current_version = get_u64(bytes, 36);
    const auto version_count = get_u32(bytes, 44);
    const auto revoked_count = get_u32(bytes, 48);
    const auto expected = 52U + static_cast<std::size_t>(version_count) * (8U + boxed_key_size) +
                          static_cast<std::size_t>(revoked_count) * 8U;
    if (output.current_version == 0 || expected != bytes.size()) {
        return std::unexpected(
            key_error(errc::corrupt, "invalid wrapped key-version table", std::string(key_id)));
    }
    std::size_t at = 52;
    for (std::uint32_t index = 0; index != version_count; ++index) {
        const auto version = get_u64(bytes, at);
        at += 8;
        if (version == 0 || output.versions.contains(version)) {
            return std::unexpected(
                key_error(errc::corrupt, "invalid wrapped key version", std::string(key_id)));
        }
        output.versions.emplace(
            version,
            wrapped_key_entry{.wrapped = {bytes.begin() + at, bytes.begin() + at + boxed_key_size},
                              .legacy = true});
        at += boxed_key_size;
    }
    for (std::uint32_t index = 0; index != revoked_count; ++index) {
        const auto version = get_u64(bytes, at);
        at += 8;
        if (version == 0 || output.versions.contains(version) ||
            !output.revoked.insert(version).second) {
            return std::unexpected(
                key_error(errc::corrupt, "invalid revoked key version", std::string(key_id)));
        }
    }
    if (!output.versions.contains(output.current_version) &&
        !output.revoked.contains(output.current_version)) {
        return std::unexpected(
            key_error(errc::corrupt, "current key version is missing", std::string(key_id)));
    }
    return output;
}

std::vector<std::byte> encode_wrapped_key_file_v2(const wrapped_key_file& wrapped) {
    std::vector<std::byte> output(key_magic_v2.begin(), key_magic_v2.end());
    output.insert(output.end(), wrapped.salt.begin(), wrapped.salt.end());
    put_u32(output, wrapped.parameters.memory_kib);
    put_u32(output, wrapped.parameters.iterations);
    put_u32(output, wrapped.parameters.lanes);
    put_u64(output, wrapped.current_version);
    put_u32(output, static_cast<std::uint32_t>(wrapped.versions.size()));
    put_u32(output, static_cast<std::uint32_t>(wrapped.revoked.size()));
    for (const auto& [version, entry] : wrapped.versions) {
        put_u64(output, version);
        output.insert(output.end(), entry.wrapped.begin(), entry.wrapped.end());
    }
    for (const auto version : wrapped.revoked)
        put_u64(output, version);
    return output;
}

std::vector<std::byte> encode_wrapped_key_file(const wrapped_key_file& wrapped) {
    std::vector<std::byte> output(key_magic_v3.begin(), key_magic_v3.end());
    put_u64(output, wrapped.current_version);
    put_u32(output, static_cast<std::uint32_t>(wrapped.versions.size()));
    put_u32(output, static_cast<std::uint32_t>(wrapped.revoked.size()));
    for (const auto& [version, entry] : wrapped.versions) {
        put_u64(output, version);
        put_u16(output, static_cast<std::uint16_t>(entry.custody.size()));
        put_u32(output, static_cast<std::uint32_t>(entry.parameters.size()));
        put_u32(output, static_cast<std::uint32_t>(entry.wrapped.size()));
        const auto custody = std::as_bytes(std::span{entry.custody});
        output.insert(output.end(), custody.begin(), custody.end());
        output.insert(output.end(), entry.parameters.begin(), entry.parameters.end());
        output.insert(output.end(), entry.wrapped.begin(), entry.wrapped.end());
    }
    for (const auto version : wrapped.revoked)
        put_u64(output, version);
    return output;
}

std::vector<std::string> encryption_limits() {
    return {"Rotation does not rewrite history.",
            "Revoking a collaborator prevents future access; it does not recall what they have.",
            "Metadata — paths, sizes, change structure and timing — remains visible unless "
            "separately protected."};
}

runtime::task<void> write_atomic(std::filesystem::path path, std::vector<std::byte> bytes) {
    const auto temporary = path.string() + ".tmp";
    auto written = co_await store_detail::write_file(temporary, std::move(bytes));
    if (!written)
        co_return std::unexpected(written.error());
    try {
#if defined(_WIN32)
        if (!MoveFileExW(std::filesystem::path{temporary}.c_str(),
                         path.c_str(),
                         MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            const auto code =
                std::error_code{static_cast<int>(GetLastError()), std::system_category()};
            co_return std::unexpected(key_error(errc::io, code.message(), path.string()));
        }
#else
        std::filesystem::rename(temporary, path);
#endif
    } catch (const std::exception& caught) {
        co_return std::unexpected(key_error(errc::io, caught.what(), path.string()));
    }
    co_return result<void>{};
}

} // namespace

auto identity_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto liquid_home() -> std::filesystem::path {
    if (const auto* configured = std::getenv("LIQUID_HOME")) {
        return configured;
    }
    if (const auto* home = std::getenv("HOME")) {
        return std::filesystem::path{home} / ".liquid";
    }
    return std::filesystem::current_path() / ".liquid";
}

auto read_bytes(const std::filesystem::path& path) -> result<std::vector<std::byte>> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            identity_error(errc::io, "cannot open identity file", path.string()));
    }
    auto output = std::vector<std::byte>{};
    for (char item; input.get(item);) {
        output.push_back(std::byte{static_cast<unsigned char>(item)});
    }
    if (!input.eof()) {
        return std::unexpected(
            identity_error(errc::io, "cannot read identity file", path.string()));
    }
    return output;
}

// Files under the identity directory are never written in place: a reader, a crash or a second
// first use must never see a partly written key (liquid#349). Each write goes to a temporary file
// unique to this writer, is flushed to disk, and only then is moved into place.
auto temporary_sibling(const std::filesystem::path& path) -> std::filesystem::path {
    thread_local auto engine = std::mt19937_64{std::random_device{}()};
#if defined(_WIN32)
    const auto process = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto process = static_cast<long>(::getpid());
#endif
    return path.string() + "." + std::to_string(process) + "." + std::to_string(engine()) + ".tmp";
}

#if defined(_WIN32)
auto write_temporary(const std::filesystem::path& temporary, std::span<const std::byte> value)
    -> result<void> {
    const auto file = CreateFileW(
        temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(
            identity_error(errc::io, "cannot write identity file", temporary.string()));
    }
    auto written = DWORD{};
    const auto ok =
        WriteFile(file, value.data(), static_cast<DWORD>(value.size()), &written, nullptr) &&
        written == value.size() && FlushFileBuffers(file);
    CloseHandle(file);
    if (!ok) {
        DeleteFileW(temporary.c_str());
        return std::unexpected(
            identity_error(errc::io, "cannot write identity file", temporary.string()));
    }
    return {};
}
#else
auto sync_directory(const std::filesystem::path& directory) -> void {
    const auto descriptor = ::open(directory.c_str(), O_RDONLY | O_CLOEXEC);
    if (descriptor < 0) {
        return;
    }
    static_cast<void>(::fsync(descriptor));
    static_cast<void>(::close(descriptor));
}

auto write_temporary(const std::filesystem::path& temporary, std::span<const std::byte> value)
    -> result<void> {
    const auto descriptor =
        ::open(temporary.c_str(), O_CREAT | O_EXCL | O_WRONLY | O_CLOEXEC, 0600);
    if (descriptor < 0) {
        return std::unexpected(identity_error(errc::io, std::strerror(errno), temporary.string()));
    }
    auto remaining = value;
    while (!remaining.empty()) {
        const auto written = ::write(descriptor, remaining.data(), remaining.size());
        if (written < 0 && errno == EINTR) {
            continue;
        }
        if (written <= 0) {
            const auto message = std::string{std::strerror(errno)};
            static_cast<void>(::close(descriptor));
            static_cast<void>(::unlink(temporary.c_str()));
            return std::unexpected(identity_error(errc::io, message, temporary.string()));
        }
        remaining = remaining.subspan(static_cast<std::size_t>(written));
    }
    if (::fsync(descriptor) != 0) {
        const auto message = std::string{std::strerror(errno)};
        static_cast<void>(::close(descriptor));
        static_cast<void>(::unlink(temporary.c_str()));
        return std::unexpected(identity_error(errc::io, message, temporary.string()));
    }
    if (::close(descriptor) != 0) {
        const auto message = std::string{std::strerror(errno)};
        static_cast<void>(::unlink(temporary.c_str()));
        return std::unexpected(identity_error(errc::io, message, temporary.string()));
    }
    return {};
}
#endif

// Writes `value` to `path`, replacing any earlier file, owner-only. A reader sees the old file or
// the whole new one, never a prefix.
auto write_private(const std::filesystem::path& path, std::span<const std::byte> value)
    -> result<void> {
    const auto temporary = temporary_sibling(path);
    auto written = write_temporary(temporary, value);
    if (!written) {
        return written;
    }
#if defined(_WIN32)
    if (!MoveFileExW(
            temporary.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return std::unexpected(
            identity_error(errc::io, "cannot write identity file", path.string()));
    }
#else
    if (::rename(temporary.c_str(), path.c_str()) != 0) {
        const auto message = std::string{std::strerror(errno)};
        static_cast<void>(::unlink(temporary.c_str()));
        return std::unexpected(identity_error(errc::io, message, path.string()));
    }
    sync_directory(path.parent_path());
#endif
    return {};
}

enum class exclusive_write { created, exists };

// Creates `path` holding `value` only if no file is there yet, owner-only, and never replaces one:
// the file appears whole or not at all. `exists` means another writer got there first; the caller
// reads theirs.
auto create_private(const std::filesystem::path& path, std::span<const std::byte> value)
    -> result<exclusive_write> {
    const auto temporary = temporary_sibling(path);
    auto written = write_temporary(temporary, value);
    if (!written) {
        return std::unexpected(written.error());
    }
#if defined(_WIN32)
    if (!MoveFileExW(temporary.c_str(), path.c_str(), MOVEFILE_WRITE_THROUGH)) {
        const auto code = GetLastError();
        DeleteFileW(temporary.c_str());
        if (code == ERROR_ALREADY_EXISTS || code == ERROR_FILE_EXISTS) {
            return exclusive_write::exists;
        }
        return std::unexpected(
            identity_error(errc::io, "cannot write identity file", path.string()));
    }
    return exclusive_write::created;
#else
    const auto linked = ::link(temporary.c_str(), path.c_str());
    const auto code = errno;
    static_cast<void>(::unlink(temporary.c_str()));
    if (linked != 0) {
        if (code == EEXIST) {
            return exclusive_write::exists;
        }
        return std::unexpected(identity_error(errc::io, std::strerror(code), path.string()));
    }
    sync_directory(path.parent_path());
    return exclusive_write::created;
#endif
}

// An exclusive lock on `<identity directory>/.lock`, held while the local identity is read or
// created, so that concurrent first uses in threads or processes create one identity between them
// (liquid#349). It is an advisory `flock` (`LockFileEx` on Windows) on a descriptor of its own, so
// it excludes other threads of this process as well as other processes, and the operating system
// drops it if the holder dies. The section it guards has no suspension point, so holding it never
// blocks a coroutine that the holder is waiting on.
class identity_lock {
public:
    static auto acquire(const std::filesystem::path& directory) -> result<identity_lock> {
        const auto path = directory / ".lock";
#if defined(_WIN32)
        const auto file = CreateFileW(path.c_str(),
                                      GENERIC_READ | GENERIC_WRITE,
                                      FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                      nullptr,
                                      OPEN_ALWAYS,
                                      FILE_ATTRIBUTE_NORMAL,
                                      nullptr);
        if (file == INVALID_HANDLE_VALUE) {
            return std::unexpected(
                identity_error(errc::io, "cannot open identity lock", path.string()));
        }
        auto overlapped = OVERLAPPED{};
        if (!LockFileEx(file, LOCKFILE_EXCLUSIVE_LOCK, 0, MAXDWORD, MAXDWORD, &overlapped)) {
            CloseHandle(file);
            return std::unexpected(
                identity_error(errc::io, "cannot lock identity directory", path.string()));
        }
        return identity_lock{file};
#else
        const auto descriptor = ::open(path.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
        if (descriptor < 0) {
            return std::unexpected(identity_error(errc::io, std::strerror(errno), path.string()));
        }
        while (::flock(descriptor, LOCK_EX) != 0) {
            const auto code = errno;
            if (code == EINTR) {
                continue;
            }
            static_cast<void>(::close(descriptor));
            return std::unexpected(identity_error(errc::io, std::strerror(code), path.string()));
        }
        return identity_lock{descriptor};
#endif
    }

    identity_lock(identity_lock&& other) noexcept
        : handle_(std::exchange(other.handle_, no_handle)) {}
    identity_lock& operator=(identity_lock&&) = delete;
    identity_lock(const identity_lock&) = delete;
    identity_lock& operator=(const identity_lock&) = delete;
    ~identity_lock() {
        release();
    }

    void release() noexcept {
        if (handle_ == no_handle) {
            return;
        }
#if defined(_WIN32)
        auto overlapped = OVERLAPPED{};
        UnlockFileEx(handle_, 0, MAXDWORD, MAXDWORD, &overlapped);
        CloseHandle(handle_);
#else
        static_cast<void>(::flock(handle_, LOCK_UN));
        static_cast<void>(::close(handle_));
#endif
        handle_ = no_handle;
    }

private:
#if defined(_WIN32)
    using native_handle = HANDLE;
    static inline const native_handle no_handle = INVALID_HANDLE_VALUE;
#else
    using native_handle = int;
    static constexpr native_handle no_handle = -1;
#endif
    explicit identity_lock(native_handle handle) noexcept : handle_(handle) {}
    native_handle handle_;
};

auto signing_key_identifier(const actor& value) -> std::string {
    const auto& fingerprint =
        value.key_fingerprint.empty() ? value.fingerprint : value.key_fingerprint;
    return "taap:1:" + value.algorithm + ":" + fingerprint;
}

auto valid_fingerprint(std::string_view value) -> bool {
    if (value.size() != 52) {
        return false;
    }
    return std::ranges::all_of(value, [](char character) {
        return (character >= 'A' && character <= 'Z') || (character >= '2' && character <= '7');
    });
}

auto valid_timestamp(std::string_view value) -> bool {
    if (value.size() != 20 || value[4] != '-' || value[7] != '-' || value[10] != 'T' ||
        value[13] != ':' || value[16] != ':' || value[19] != 'Z') {
        return false;
    }
    for (const auto index : {0U, 1U, 2U, 3U, 5U, 6U, 8U, 9U, 11U, 12U, 14U, 15U, 17U, 18U}) {
        if (!std::isdigit(static_cast<unsigned char>(value[index]))) {
            return false;
        }
    }
    const auto number = [&](std::size_t offset, std::size_t count) {
        return std::stoi(std::string{value.substr(offset, count)});
    };
    const auto day = std::chrono::year{number(0, 4)} /
                     std::chrono::month{static_cast<unsigned>(number(5, 2))} /
                     std::chrono::day{static_cast<unsigned>(number(8, 2))};
    return day.ok() && number(11, 2) < 24 && number(14, 2) < 60 && number(17, 2) < 60;
}

auto next_timestamp(std::string_view previous) -> result<std::string> {
    if (!valid_timestamp(previous)) {
        return std::unexpected(
            identity_error(errc::invalid_argument, "invalid previous update time", "updated_at"));
    }
    auto current = identity_detail::now();
    if (current > previous) {
        return current;
    }
    const auto number = [&](std::size_t offset, std::size_t count) {
        return std::stoi(std::string{previous.substr(offset, count)});
    };
    const auto date = std::chrono::year{number(0, 4)} /
                      std::chrono::month{static_cast<unsigned>(number(5, 2))} /
                      std::chrono::day{static_cast<unsigned>(number(8, 2))};
    const auto value = std::chrono::sys_days{date} + std::chrono::hours{number(11, 2)} +
                       std::chrono::minutes{number(14, 2)} + std::chrono::seconds{number(17, 2)} +
                       std::chrono::seconds{1};
    return std::format("{:%FT%TZ}", value);
}

auto document_directory(const actor& subject) -> std::filesystem::path {
    return liquid_home() / "identity" / subject.fingerprint / "documents";
}

auto document_path(const std::filesystem::path& directory, std::size_t index)
    -> std::filesystem::path {
    return directory / std::format("{:03}.cbor", index);
}

struct stored_document {
    identity_document document;
    std::vector<std::byte> bytes;
};

auto load_stored_documents(const actor& subject) -> result<std::vector<stored_document>> {
    if (!valid_fingerprint(subject.fingerprint)) {
        return std::unexpected(identity_error(
            errc::invalid_argument, "invalid identity fingerprint", subject.taap_id));
    }
    const auto directory = document_directory(subject);
    auto paths = std::vector<std::filesystem::path>{};
    std::error_code code;
    if (std::filesystem::exists(directory, code)) {
        if (code) {
            return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
        }
        for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
            if (code) {
                return std::unexpected(
                    identity_error(errc::io, code.message(), directory.string()));
            }
            if (entry.is_regular_file() && entry.path().extension() == ".cbor") {
                paths.push_back(entry.path());
            }
        }
    }
    if (paths.empty()) {
        const auto legacy = liquid_home() / "identity" / (subject.fingerprint + ".identity.cbor");
        if (std::filesystem::exists(legacy, code) && !code) {
            paths.push_back(legacy);
        }
    }
    std::ranges::sort(paths);
    auto output = std::vector<stored_document>{};
    for (const auto& path : paths) {
        auto bytes = read_bytes(path);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        auto document = identity_detail::parse_document(*bytes);
        if (!document) {
            return std::unexpected(document.error());
        }
        output.push_back({std::move(*document), std::move(*bytes)});
    }
    if (output.empty()) {
        return std::unexpected(identity_error(
            errc::not_found, "identity document chain is unavailable", subject.taap_id));
    }
    return output;
}

auto read_wrapping_key(const std::filesystem::path& path) -> result<std::array<std::byte, 32>> {
    auto saved = read_bytes(path);
    if (!saved || saved->size() != 32) {
        if (saved) {
            crypto_wipe(saved->data(), saved->size());
        }
        return std::unexpected(identity_error(
            errc::unauthorized, "invalid local identity key provider", path.string()));
    }
    auto output = std::array<std::byte, 32>{};
    std::ranges::copy(*saved, output.begin());
    crypto_wipe(saved->data(), saved->size());
    return output;
}

// Reads the local wrapping key, creating it if there is none. Creation never replaces a key that is
// already there: if another writer creates one first, theirs is read and used (liquid#349).
auto identity_wrapping_key(const std::filesystem::path& directory)
    -> result<std::array<std::byte, 32>> {
    const auto path = directory / ".wrapping.key";
    if (std::filesystem::exists(path)) {
        return read_wrapping_key(path);
    }
    auto output = std::array<std::byte, 32>{};
    auto random = crypto_detail::random_bytes(output);
    if (!random) {
        return std::unexpected(random.error());
    }
    auto created = create_private(path, output);
    if (!created) {
        crypto_wipe(output.data(), output.size());
        return std::unexpected(created.error());
    }
    if (*created == exclusive_write::exists) {
        crypto_wipe(output.data(), output.size());
        return read_wrapping_key(path);
    }
    return output;
}

struct signer::impl {
    actor identity;
    identity_document document;
    std::string label;
    std::unique_ptr<identity_detail::key_backend> backend;
};

auto actor::identifier() const noexcept -> std::string_view {
    return taap_id;
}

result<std::vector<std::byte>> encode_identity_document(const identity_document& document) {
    return identity_detail::canonical_document(document, true);
}

result<identity_document> decode_identity_document(std::span<const std::byte> encoded) {
    return identity_detail::parse_document(encoded);
}

result<signature> decode_signature(std::span<const std::byte> encoded) {
    return identity_detail::parse_signature(encoded);
}

signer::signer() noexcept = default;
signer::~signer() = default;
signer::signer(signer&&) noexcept = default;
signer& signer::operator=(signer&&) noexcept = default;

runtime::task<signature> signer::sign(std::string artifact_type,
                                      std::vector<std::byte> canonical_body) {
    if (!impl_ || artifact_type.empty()) {
        co_return std::unexpected(identity_error(errc::invalid_argument, "signer has no identity"));
    }
    auto input = identity_detail::signing_input(artifact_type, canonical_body);
    auto signed_bytes = co_await impl_->backend->sign(std::move(input));
    if (!signed_bytes) {
        co_return std::unexpected(signed_bytes.error());
    }
    co_return signature{std::move(signed_bytes->algorithm),
                        signing_key_identifier(impl_->identity),
                        impl_->label,
                        std::move(artifact_type),
                        identity_detail::now(),
                        std::move(signed_bytes->bytes),
                        std::move(signed_bytes->authenticator_data),
                        std::move(signed_bytes->client_data_hash)};
}

auto signer::identity() const -> actor {
    return impl_ ? impl_->identity : actor{};
}

auto signer::document() const -> result<identity_document> {
    if (!impl_) {
        return std::unexpected(identity_error(errc::invalid_argument, "signer has no identity"));
    }
    return impl_->document;
}

runtime::task<signer> signer::open(std::string identifier, keyring&) {
    auto opened = co_await identity_detail::local_signer();
    if (!opened) {
        co_return std::unexpected(opened.error());
    }
    if (opened->identity().taap_id != identifier) {
        co_return std::unexpected(
            identity_error(errc::not_found, "identity is not available", identifier));
    }
    co_return std::move(*opened);
}

namespace {

auto valid_compromise_declaration(const compromise_declaration& declaration) -> bool;

} // namespace

runtime::task<identity_document> signer::rotate(std::optional<compromise_declaration> compromise) {
    co_return co_await rotate_to("software", std::move(compromise));
}

runtime::task<identity_document>
signer::rotate_to(std::string custody, std::optional<compromise_declaration> compromise) {
    if (!impl_) {
        co_return std::unexpected(identity_error(errc::invalid_argument, "signer has no identity"));
    }
    auto stored = load_stored_documents(impl_->identity);
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    auto documents = std::vector<identity_document>{};
    documents.reserve(stored->size());
    for (const auto& document : *stored) {
        documents.push_back(document.document);
    }
    auto verified = verify_chain(documents);
    if (!verified) {
        co_return std::unexpected(verified.error());
    }
    const auto& current = stored->back().document;
    if (current.subject.taap_id != impl_->identity.taap_id ||
        current.subject.key_fingerprint != impl_->identity.key_fingerprint) {
        co_return std::unexpected(identity_error(
            errc::unauthorized, "signer is not the current identity key", impl_->identity.taap_id));
    }
    if (compromise && compromise->key_fingerprint_base32 != current.subject.key_fingerprint) {
        co_return std::unexpected(identity_error(errc::invalid_argument,
                                                 "compromise declaration must name the current key",
                                                 impl_->identity.taap_id));
    }
    if (compromise && !valid_compromise_declaration(*compromise)) {
        co_return std::unexpected(identity_error(
            errc::invalid_argument, "invalid compromise declaration", impl_->identity.taap_id));
    }
    auto updated_at = next_timestamp(current.updated_at);
    if (!updated_at) {
        co_return std::unexpected(updated_at.error());
    }
    const auto label = impl_->label + "-rotated-" + std::to_string(stored->size());
    auto new_backend = result<std::unique_ptr<identity_detail::key_backend>>{std::unexpected(
        identity_error(errc::invalid_argument, "unsupported identity key custody", custody))};
    if (custody == "software") {
        new_backend = identity_detail::make_software_backend();
    } else if (custody == "fido2") {
        new_backend = co_await identity_detail::make_fido2_backend();
    } else if (custody == "piv:9c") {
        new_backend = co_await identity_detail::make_piv_backend("9c");
    }
    if (!new_backend) {
        co_return std::unexpected(new_backend.error());
    }
    auto new_identity = identity_detail::identifier((*new_backend)->public_key(),
                                                    *updated_at,
                                                    label,
                                                    (*new_backend)->algorithm(),
                                                    (*new_backend)->custody());
    if (!new_identity) {
        co_return std::unexpected(new_identity.error());
    }
    new_identity->taap_id = current.subject.taap_id;
    new_identity->fingerprint = current.subject.fingerprint;
    const auto predecessor_hash = identity_detail::sha512_256(stored->back().bytes);
    identity_document successor{
        std::move(*new_identity),
        label,
        *updated_at,
        *updated_at,
        std::vector<std::byte>{predecessor_hash.begin(), predecessor_hash.end()},
        {},
        {}};
    if (compromise) {
        successor.compromised_keys.push_back(std::move(*compromise));
    }
    auto body = identity_detail::canonical_document(successor, false);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto directory = liquid_home() / "identity";
    std::error_code code;
    std::filesystem::create_directories(document_directory(current.subject), code);
    if (code) {
        co_return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    for (std::size_t index = 0; index < stored->size(); ++index) {
        const auto path = document_path(document_directory(current.subject), index);
        if (std::filesystem::exists(path, code)) {
            if (code) {
                co_return std::unexpected(identity_error(errc::io, code.message(), path.string()));
            }
            continue;
        }
        auto copied = write_private(path, (*stored)[index].bytes);
        if (!copied) {
            co_return std::unexpected(copied.error());
        }
    }
    auto persisted = (*new_backend)->descriptor();
    if ((*new_backend)->custody() == "software") {
        auto wrapping_key = identity_wrapping_key(directory);
        if (!wrapping_key) {
            crypto_wipe(persisted.data(), persisted.size());
            co_return std::unexpected(wrapping_key.error());
        }
        auto sealed = crypto_detail::seal(*wrapping_key, {}, persisted);
        crypto_wipe(wrapping_key->data(), wrapping_key->size());
        crypto_wipe(persisted.data(), persisted.size());
        if (!sealed) {
            co_return std::unexpected(sealed.error());
        }
        persisted = std::move(*sealed);
    }
    if (successor.compromised_keys.empty()) {
        auto old_signature = co_await sign("identity_document", *body);
        if (!old_signature) {
            crypto_wipe(persisted.data(), persisted.size());
            co_return std::unexpected(old_signature.error());
        }
        successor.signatures.push_back(std::move(*old_signature));
    }
    signer next;
    next.impl_ = std::make_unique<signer::impl>();
    next.impl_->identity = successor.subject;
    next.impl_->label = successor.key_label;
    next.impl_->backend = std::move(*new_backend);
    auto new_signature = co_await next.sign("identity_document", *body);
    if (!new_signature) {
        crypto_wipe(persisted.data(), persisted.size());
        co_return std::unexpected(new_signature.error());
    }
    successor.signatures.push_back(std::move(*new_signature));
    auto encoded = identity_detail::canonical_document(successor, true);
    if (!encoded) {
        crypto_wipe(persisted.data(), persisted.size());
        co_return std::unexpected(encoded.error());
    }
    auto written = write_private(directory / (current.subject.fingerprint + ".key"), persisted);
    crypto_wipe(persisted.data(), persisted.size());
    if (!written) {
        co_return std::unexpected(written.error());
    }
    written = write_private(directory / (current.subject.fingerprint + ".pub"),
                            successor.subject.public_key);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    written =
        write_private(document_path(document_directory(current.subject), stored->size()), *encoded);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    written = write_private(directory / (current.subject.fingerprint + ".identity.cbor"), *encoded);
    if (!written) {
        co_return std::unexpected(written.error());
    }
    impl_->identity = successor.subject;
    impl_->document = successor;
    impl_->label = successor.key_label;
    impl_->backend = std::move(next.impl_->backend);
    co_return successor;
}

namespace {

// WebAuthn Level 2 §6.3.3 step 11 and CTAP 2.2 authenticatorGetAssertion: the authenticator signs
// the concatenation authenticatorData || clientDataHash, where clientDataHash is the 32 bytes the
// client sent (here SHA-256 of the TAAP signed bytes). The client hash is not hashed again.
// EdDSA signs this message directly; ES256 is ECDSA P-256 over SHA-256 of it.
auto fido2_assertion_input(const signature& signed_value) -> std::vector<std::byte> {
    auto assertion = signed_value.authenticator_data;
    assertion.insert(assertion.end(),
                     signed_value.client_data_hash.begin(),
                     signed_value.client_data_hash.end());
    return assertion;
}

// WebAuthn Level 2 §7.2 steps 15-16: authenticatorData starts with SHA-256 of the relying-party
// identifier ("liquid", the rpId liquid's CTAP2 requests name) and its flags carry user presence.
auto valid_fido2_authenticator_data(std::span<const std::byte> authenticator_data) -> bool {
    constexpr auto rp_id = std::string_view{"liquid"};
    constexpr auto user_present = std::byte{0x01};
    if (authenticator_data.size() < 37 || (authenticator_data[32] & user_present) != user_present) {
        return false;
    }
    auto rp_id_hash = crypto::digest(crypto::digest_kind::sha256, std::as_bytes(std::span{rp_id}));
    return rp_id_hash && std::ranges::equal(*rp_id_hash, authenticator_data.first(32));
}

auto verify_with_key(const actor& by,
                     std::string_view artifact_type,
                     std::span<const std::byte> canonical_body,
                     const signature& signed_value) -> result<bool> {
    if (signed_value.bytes.empty()) {
        return false;
    }
    if (artifact_type.empty() || signed_value.algorithm != by.algorithm ||
        signed_value.signing_key_identifier != signing_key_identifier(by) ||
        signed_value.signature_purpose != artifact_type) {
        return std::unexpected(
            identity_error(errc::unauthorized, "invalid TAAP signature metadata"));
    }
    auto input = identity_detail::signing_input(artifact_type, canonical_body);
    auto verified = result<void>{};
    if ((by.algorithm == "ed25519" || by.algorithm == "fido2-eddsa") &&
        by.public_key.size() == 32 && signed_value.bytes.size() == 64) {
        auto message = std::span<const std::byte>{input};
        auto assertion = std::vector<std::byte>{};
        if (by.algorithm == "fido2-eddsa") {
            auto client_data = crypto::digest(crypto::digest_kind::sha256, input);
            if (!client_data || signed_value.client_data_hash != *client_data ||
                !valid_fido2_authenticator_data(signed_value.authenticator_data)) {
                crypto_wipe(input.data(), input.size());
                return std::unexpected(
                    identity_error(errc::unauthorized, "invalid FIDO2 signature metadata"));
            }
            assertion = fido2_assertion_input(signed_value);
            message = assertion;
        } else if (!signed_value.authenticator_data.empty() ||
                   !signed_value.client_data_hash.empty()) {
            crypto_wipe(input.data(), input.size());
            return std::unexpected(
                identity_error(errc::unauthorized, "invalid TAAP signature metadata"));
        }
        if (crypto_ed25519_check(reinterpret_cast<const std::uint8_t*>(signed_value.bytes.data()),
                                 reinterpret_cast<const std::uint8_t*>(by.public_key.data()),
                                 reinterpret_cast<const std::uint8_t*>(message.data()),
                                 message.size()) != 0) {
            verified = std::unexpected(
                identity_error(errc::unauthorized, "TAAP signature verification failed"));
        }
        crypto_wipe(assertion.data(), assertion.size());
    } else if ((by.algorithm == "ecdsa-p256" || by.algorithm == "fido2-es256") &&
               by.public_key.size() == 64) {
        auto message = std::span<const std::byte>{input};
        auto assertion = std::vector<std::byte>{};
        if (by.algorithm == "fido2-es256") {
            auto client_data = crypto::digest(crypto::digest_kind::sha256, input);
            if (!client_data || signed_value.client_data_hash != *client_data ||
                !valid_fido2_authenticator_data(signed_value.authenticator_data)) {
                crypto_wipe(input.data(), input.size());
                return std::unexpected(
                    identity_error(errc::unauthorized, "invalid FIDO2 signature metadata"));
            }
            assertion = fido2_assertion_input(signed_value);
            message = assertion;
        } else if (!signed_value.authenticator_data.empty() ||
                   !signed_value.client_data_hash.empty()) {
            crypto_wipe(input.data(), input.size());
            return std::unexpected(
                identity_error(errc::unauthorized, "invalid TAAP signature metadata"));
        }
        auto key = crypto::public_key{.kind = crypto::signature_kind::ecdsa,
                                      .curve = crypto::curve_kind::p256,
                                      .first = {by.public_key.begin(), by.public_key.begin() + 32},
                                      .second = {by.public_key.begin() + 32, by.public_key.end()}};
        verified = crypto::verify_signature(
            key,
            {.kind = crypto::signature_kind::ecdsa, .digest = crypto::digest_kind::sha256},
            message,
            signed_value.bytes);
        crypto_wipe(assertion.data(), assertion.size());
    } else {
        crypto_wipe(input.data(), input.size());
        return std::unexpected(
            identity_error(errc::unauthorized, "invalid TAAP signature metadata"));
    }
    crypto_wipe(input.data(), input.size());
    if (!verified) {
        return std::unexpected(
            identity_error(errc::unauthorized, "TAAP signature verification failed"));
    }
    return true;
}

auto chain_failure(std::string name) -> error {
    return identity_error(errc::unauthorized, std::move(name), "identity chain");
}

auto valid_compromise_declaration(const compromise_declaration& declaration) -> bool {
    return valid_fingerprint(declaration.key_fingerprint_base32) &&
           valid_timestamp(declaration.declared_at);
}

auto valid_subject(const identity_document& document) -> bool {
    const auto algorithm = document.subject.algorithm;
    const auto valid_key = ((algorithm == "ed25519" || algorithm == "fido2-eddsa") &&
                            document.subject.public_key.size() == 32) ||
                           ((algorithm == "ecdsa-p256" || algorithm == "fido2-es256") &&
                            document.subject.public_key.size() == 64);
    const auto valid_custody =
        (algorithm == "ed25519" &&
         (document.subject.custody.empty() || document.subject.custody == "software")) ||
        ((algorithm == "fido2-eddsa" || algorithm == "fido2-es256") &&
         document.subject.custody == "fido2") ||
        (algorithm == "ecdsa-p256" && document.subject.custody == "piv:9c");
    if (!valid_key || !valid_custody || !valid_fingerprint(document.subject.fingerprint) ||
        !valid_fingerprint(document.subject.key_fingerprint) ||
        !document.subject.taap_id.ends_with(document.subject.fingerprint)) {
        return false;
    }
    auto expected = identity_detail::key_fingerprint(document.subject.public_key,
                                                     document.created_at,
                                                     document.key_label,
                                                     document.subject.algorithm,
                                                     document.subject.custody);
    return expected && *expected == document.subject.key_fingerprint;
}

auto declares_compromised(const identity_document& document, std::string_view fingerprint)
    -> const compromise_declaration* {
    for (const auto& declaration : document.compromised_keys) {
        if (declaration.key_fingerprint_base32 == fingerprint) {
            return &declaration;
        }
    }
    return nullptr;
}

// An identifier is the fingerprint of its identity's initial key. That is the only thing that ties
// a name to a key without a third party, so an initial document must name its own key.
auto names_its_own_key(const identity_document& document) -> bool {
    return document.subject.fingerprint == document.subject.key_fingerprint &&
           document.subject.taap_id ==
               "taap:1:" + document.subject.algorithm + ":" + document.subject.fingerprint;
}

// A rotation proves continuity of control only when the key it replaces signed it. One that
// carries just a compromise declaration was signed by its new key alone.
auto signed_by_replaced_key(const identity_document& previous, const identity_document& current)
    -> bool {
    const auto replaced = signing_key_identifier(previous.subject);
    return std::ranges::any_of(current.signatures, [&](const signature& value) {
        return value.signing_key_identifier == replaced;
    });
}

auto first_unproven_rotation(std::span<const identity_document> documents, std::size_t from)
    -> std::optional<std::size_t> {
    for (auto index = std::max<std::size_t>(from, 1); index < documents.size(); ++index) {
        if (!signed_by_replaced_key(documents[index - 1], documents[index])) {
            return index;
        }
    }
    return std::nullopt;
}

constexpr std::string_view unknown_signer_message =
    "unknown signer: no identity chain binds this name to a key";

auto unknown_signer(std::string_view taap_id) -> error {
    return identity_error(
        errc::unauthorized, std::string{unknown_signer_message}, std::string{taap_id});
}

// The identifier fingerprint is the last field of `taap:1:<algorithm>:<fingerprint>`.
auto name_fingerprint(std::string_view taap_id) -> std::optional<std::string> {
    constexpr std::string_view prefix = "taap:1:";
    const auto separator = taap_id.rfind(':');
    if (!taap_id.starts_with(prefix) || separator == std::string_view::npos ||
        separator < prefix.size()) {
        return std::nullopt;
    }
    const auto fingerprint = taap_id.substr(separator + 1);
    if (!valid_fingerprint(fingerprint)) {
        return std::nullopt;
    }
    return std::string{fingerprint};
}

auto stored_chain(std::string_view taap_id) -> result<std::vector<stored_document>> {
    const auto fingerprint = name_fingerprint(taap_id);
    if (!fingerprint) {
        return std::unexpected(unknown_signer(taap_id));
    }
    auto subject = actor{};
    subject.taap_id = taap_id;
    subject.fingerprint = *fingerprint;
    auto stored = load_stored_documents(subject);
    if (!stored && stored.error().code == errc::not_found) {
        return std::unexpected(unknown_signer(taap_id));
    }
    return stored;
}

auto verify_against_chain(const actor& by,
                          std::span<const identity_document> documents,
                          bool require_continuity,
                          std::string_view artifact_type,
                          std::span<const std::byte> canonical_body,
                          const signature& signed_value) -> result<bool> {
    if (documents.empty()) {
        return std::unexpected(unknown_signer(by.taap_id));
    }
    auto chain = verify_chain(documents);
    if (!chain) {
        return std::unexpected(chain.error());
    }
    const auto& initial = documents.front().subject;
    if (by.taap_id != initial.taap_id ||
        (!by.fingerprint.empty() && by.fingerprint != initial.fingerprint)) {
        return std::unexpected(identity_error(
            errc::unauthorized, "signer name does not match its identity chain", by.taap_id));
    }
    if (require_continuity && first_unproven_rotation(documents, 1)) {
        return std::unexpected(
            identity_error(errc::unauthorized,
                           "identity chain has a rotation not signed by the key it replaces",
                           by.taap_id));
    }
    for (std::size_t index = 0; index < documents.size(); ++index) {
        const auto& candidate = documents[index].subject;
        if (signed_value.signing_key_identifier != signing_key_identifier(candidate)) {
            continue;
        }
        for (std::size_t successor = index + 1; successor < documents.size(); ++successor) {
            const auto* declaration =
                declares_compromised(documents[successor], candidate.key_fingerprint);
            if (declaration) {
                if (!valid_timestamp(signed_value.signed_at) ||
                    signed_value.signed_at >= declaration->declared_at) {
                    return std::unexpected(identity_error(errc::unauthorized,
                                                          "signature was made after key compromise",
                                                          candidate.taap_id));
                }
            }
        }
        return verify_with_key(candidate, artifact_type, canonical_body, signed_value);
    }
    return std::unexpected(
        identity_error(errc::unauthorized, "signing key is not in the identity chain", by.taap_id));
}

} // namespace

result<bool> verify(const actor& by,
                    std::string_view artifact_type,
                    std::span<const std::byte> canonical_body,
                    const signature& signed_value) {
    auto stored = stored_chain(by.taap_id);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    auto documents = std::vector<identity_document>{};
    documents.reserve(stored->size());
    for (auto& document : *stored) {
        documents.push_back(std::move(document.document));
    }
    return verify_against_chain(by, documents, false, artifact_type, canonical_body, signed_value);
}

result<bool> verify(const actor& by,
                    std::span<const identity_document> chain,
                    std::string_view artifact_type,
                    std::span<const std::byte> canonical_body,
                    const signature& signed_value) {
    return verify_against_chain(by, chain, true, artifact_type, canonical_body, signed_value);
}

result<bool> verify_key_signature(const actor& key,
                                  std::string_view artifact_type,
                                  std::span<const std::byte> canonical_body,
                                  const signature& signed_value) {
    return verify_with_key(key, artifact_type, canonical_body, signed_value);
}

bool is_unknown_signer(const error& value) noexcept {
    return value.code == errc::unauthorized && value.message == unknown_signer_message;
}

result<signature_check> check_signature(const actor& by,
                                        std::string_view artifact_type,
                                        std::span<const std::byte> canonical_body,
                                        const signature& signed_value) {
    auto bound = verify(by, artifact_type, canonical_body, signed_value);
    if (bound) {
        return signature_check{.signature_valid = *bound, .identity_verified = *bound};
    }
    if (!is_unknown_signer(bound.error())) {
        return std::unexpected(bound.error());
    }
    // No chain binds the name, so the record's own key can show only that some key signed. The
    // identity stays unverified whatever this says (liquid#429).
    auto key_only = verify_with_key(by, artifact_type, canonical_body, signed_value);
    if (!key_only) {
        return std::unexpected(key_only.error());
    }
    return signature_check{.signature_valid = *key_only, .identity_verified = false};
}

result<bool> verify_document(const identity_document& document) {
    if (!valid_subject(document) || document.signatures.empty() ||
        (!document.previous_document_hash && !names_its_own_key(document))) {
        return std::unexpected(identity_error(errc::unauthorized, "invalid identity document"));
    }
    auto body = identity_detail::canonical_document(document, false);
    if (!body) {
        return std::unexpected(body.error());
    }
    for (const auto& signed_value : document.signatures) {
        if (signed_value.signing_key_identifier != signing_key_identifier(document.subject)) {
            continue;
        }
        auto valid = verify_with_key(document.subject, "identity_document", *body, signed_value);
        if (valid && *valid) {
            return true;
        }
        return valid;
    }
    return std::unexpected(identity_error(
        errc::unauthorized, "identity document lacks a self-signature", document.subject.taap_id));
}

result<verified_identity> verify_chain(std::span<const identity_document> documents) {
    if (documents.empty()) {
        return std::unexpected(chain_failure("identity_chain_empty"));
    }
    const auto& first = documents.front();
    if (first.previous_document_hash) {
        return std::unexpected(chain_failure("identity_chain_unexpected_predecessor"));
    }
    if (!valid_timestamp(first.created_at) || !valid_timestamp(first.updated_at) ||
        first.updated_at < first.created_at || !valid_subject(first)) {
        return std::unexpected(chain_failure("identity_chain_invalid_document"));
    }
    if (!names_its_own_key(first)) {
        return std::unexpected(chain_failure("identity_chain_identifier_not_derived_from_key"));
    }
    auto self_signed = verify_document(first);
    if (!self_signed || !*self_signed) {
        return std::unexpected(chain_failure("identity_chain_invalid_self_signature"));
    }
    auto compromised_history = false;
    for (const auto& declaration : first.compromised_keys) {
        if (!valid_compromise_declaration(declaration)) {
            return std::unexpected(chain_failure("identity_chain_invalid_compromise_declaration"));
        }
        compromised_history = true;
    }
    for (std::size_t index = 1; index < documents.size(); ++index) {
        const auto& previous = documents[index - 1];
        const auto& current = documents[index];
        if (current.subject.taap_id != first.subject.taap_id ||
            current.subject.fingerprint != first.subject.fingerprint || !valid_subject(current)) {
            return std::unexpected(chain_failure("identity_chain_identifier_changed"));
        }
        if (!valid_timestamp(current.created_at) || !valid_timestamp(current.updated_at) ||
            current.updated_at <= previous.updated_at) {
            return std::unexpected(chain_failure("identity_chain_non_monotonic_updated_at"));
        }
        auto previous_bytes = identity_detail::canonical_document(previous, true);
        if (!previous_bytes) {
            return std::unexpected(previous_bytes.error());
        }
        const auto expected_hash = identity_detail::sha512_256(*previous_bytes);
        if (!current.previous_document_hash ||
            !std::ranges::equal(*current.previous_document_hash, expected_hash)) {
            return std::unexpected(chain_failure("identity_chain_broken_predecessor_hash"));
        }
        for (const auto& declaration : current.compromised_keys) {
            if (!valid_compromise_declaration(declaration)) {
                return std::unexpected(
                    chain_failure("identity_chain_invalid_compromise_declaration"));
            }
            compromised_history = true;
        }
        auto body = identity_detail::canonical_document(current, false);
        if (!body) {
            return std::unexpected(body.error());
        }
        auto new_key_signature = false;
        auto old_key_signature = false;
        for (const auto& signed_value : current.signatures) {
            if (signed_value.signing_key_identifier == signing_key_identifier(current.subject)) {
                auto valid =
                    verify_with_key(current.subject, "identity_document", *body, signed_value);
                if (!valid || !*valid) {
                    return std::unexpected(
                        chain_failure("identity_chain_invalid_new_key_signature"));
                }
                new_key_signature = true;
            }
            if (signed_value.signing_key_identifier == signing_key_identifier(previous.subject)) {
                auto valid =
                    verify_with_key(previous.subject, "identity_document", *body, signed_value);
                if (!valid || !*valid) {
                    return std::unexpected(
                        chain_failure("identity_chain_invalid_old_key_signature"));
                }
                old_key_signature = true;
            }
        }
        if (!new_key_signature) {
            return std::unexpected(chain_failure("identity_chain_missing_new_key_signature"));
        }
        if (!declares_compromised(current, previous.subject.key_fingerprint) &&
            !old_key_signature) {
            return std::unexpected(chain_failure("identity_chain_missing_dual_signature"));
        }
    }
    return verified_identity{
        documents.back().subject, {documents.back().subject}, compromised_history};
}

result<verified_identity> trust_identity_chain(std::span<const identity_document> chain,
                                               identity_trust_options options) {
    auto verified = verify_chain(chain);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    const auto& subject = chain.front().subject;
    auto encoded = std::vector<std::vector<std::byte>>{};
    encoded.reserve(chain.size());
    for (const auto& document : chain) {
        auto bytes = identity_detail::canonical_document(document, true);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        encoded.push_back(std::move(*bytes));
    }
    auto stored = load_stored_documents(subject);
    if (!stored && stored.error().code != errc::not_found) {
        return std::unexpected(stored.error());
    }
    const auto known = stored ? stored->size() : std::size_t{};
    for (std::size_t index = 0; index < std::min(known, encoded.size()); ++index) {
        if ((*stored)[index].bytes != encoded[index]) {
            return std::unexpected(identity_error(
                errc::conflict, "identity chain differs from the stored chain", subject.taap_id));
        }
    }
    if (encoded.size() <= known) {
        auto documents = std::vector<identity_document>{};
        documents.reserve(stored->size());
        for (const auto& document : *stored) {
            documents.push_back(document.document);
        }
        return verify_chain(documents);
    }
    if (!options.accept_compromise_rotation && first_unproven_rotation(chain, known)) {
        return std::unexpected(
            identity_error(errc::unauthorized,
                           "identity chain has a rotation not signed by the key it replaces",
                           subject.taap_id));
    }
    const auto directory = document_directory(subject);
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    for (auto index = known; index < encoded.size(); ++index) {
        auto written = write_private(document_path(directory, index), encoded[index]);
        if (!written) {
            return std::unexpected(written.error());
        }
    }
    return verified;
}

result<std::vector<identity_document>> load_identity_chain(std::string_view taap_id) {
    auto stored = stored_chain(taap_id);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    auto documents = std::vector<identity_document>{};
    documents.reserve(stored->size());
    for (auto& document : *stored) {
        documents.push_back(std::move(document.document));
    }
    return documents;
}

result<std::vector<std::byte>> encode_identity_chain(std::span<const identity_document> chain) {
    auto items = encoding::cbor::value::array{};
    items.reserve(chain.size());
    for (const auto& document : chain) {
        auto bytes = identity_detail::canonical_document(document, true);
        if (!bytes) {
            return std::unexpected(bytes.error());
        }
        items.emplace_back(std::move(*bytes));
    }
    return encoding::cbor::encode(encoding::cbor::value{std::move(items)});
}

result<std::vector<identity_document>> decode_identity_chain(std::span<const std::byte> encoded) {
    auto decoded = encoding::cbor::decode(encoded);
    if (!decoded) {
        return std::unexpected(decoded.error());
    }
    const auto* items = std::get_if<encoding::cbor::value::array>(&decoded->data);
    if (!items || items->empty()) {
        return std::unexpected(
            identity_error(errc::invalid_argument, "identity chain is not a document array"));
    }
    auto output = std::vector<identity_document>{};
    output.reserve(items->size());
    for (const auto& item : *items) {
        const auto* bytes = std::get_if<encoding::cbor::value::bytes>(&item.data);
        if (!bytes) {
            return std::unexpected(
                identity_error(errc::invalid_argument, "identity chain entry is not bytes"));
        }
        auto document = identity_detail::parse_document(*bytes);
        if (!document) {
            return std::unexpected(document.error());
        }
        output.push_back(std::move(*document));
    }
    auto reencoded = encode_identity_chain(output);
    if (!reencoded || !std::ranges::equal(*reencoded, encoded)) {
        return std::unexpected(
            identity_error(errc::invalid_argument, "identity chain is not canonical"));
    }
    return output;
}

namespace {

struct local_identity {
    actor identity;
    identity_document document;
    std::string label;
    std::unique_ptr<identity_detail::key_backend> backend;
};

// Reads the local identity, if one has been created. The `.pub` file is written last when an
// identity is created, so its presence means every other file is already in place.
auto load_local_identity(const std::filesystem::path& directory)
    -> result<std::optional<local_identity>> {
    auto public_key = std::vector<std::byte>{};
    auto fingerprint = std::string{};
    std::error_code code;
    for (const auto& entry : std::filesystem::directory_iterator(directory, code)) {
        if (entry.path().extension() == ".pub") {
            public_key = read_bytes(entry.path()).value_or(std::vector<std::byte>{});
            fingerprint = entry.path().stem().string();
            break;
        }
    }
    if (code) {
        return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    if (public_key.empty() || fingerprint.empty()) {
        return std::optional<local_identity>{};
    }
    auto provisional =
        actor{"taap:1:ed25519:" + fingerprint, "ed25519", fingerprint, public_key, {}, {}};
    auto stored = load_stored_documents(provisional);
    if (!stored) {
        return std::unexpected(stored.error());
    }
    auto documents = std::vector<identity_document>{};
    documents.reserve(stored->size());
    for (const auto& document : *stored) {
        documents.push_back(document.document);
    }
    auto verified = verify_chain(documents);
    if (!verified) {
        return std::unexpected(verified.error());
    }
    auto output = local_identity{};
    output.identity = stored->back().document.subject;
    output.label = stored->back().document.key_label;
    output.document = stored->back().document;
    if (output.identity.public_key != public_key) {
        return std::unexpected(identity_error(errc::corrupt,
                                              "current identity document does not match public key",
                                              output.identity.taap_id));
    }
    auto saved = read_bytes(directory / (fingerprint + ".key"));
    if (!saved) {
        return std::unexpected(saved.error());
    }
    if (output.identity.custody.empty() || output.identity.custody == "software") {
        auto wrapping_key = identity_wrapping_key(directory);
        if (!wrapping_key) {
            crypto_wipe(saved->data(), saved->size());
            return std::unexpected(wrapping_key.error());
        }
        auto opened = crypto_detail::open(*wrapping_key, {}, *saved);
        crypto_wipe(wrapping_key->data(), wrapping_key->size());
        crypto_wipe(saved->data(), saved->size());
        if (!opened) {
            return std::unexpected(
                identity_error(errc::unauthorized, "cannot unwrap local identity"));
        }
        auto made = identity_detail::make_software_backend(*opened);
        crypto_wipe(opened->data(), opened->size());
        if (!made) {
            return std::unexpected(made.error());
        }
        output.backend = std::move(*made);
    } else {
        auto made = identity_detail::open_hardware_backend(output.identity.custody, *saved);
        crypto_wipe(saved->data(), saved->size());
        if (!made) {
            return std::unexpected(made.error());
        }
        output.backend = std::move(*made);
    }
    return std::optional<local_identity>{std::move(output)};
}

auto load_local_identity_locked(const std::filesystem::path& directory)
    -> result<std::optional<local_identity>> {
    auto lock = identity_lock::acquire(directory);
    if (!lock) {
        return std::unexpected(lock.error());
    }
    return load_local_identity(directory);
}

// Writes a newly made identity into the directory. Called with the identity lock held, after
// checking that no identity is there. The private key is sealed with the directory's wrapping key
// (an existing one is reused, never replaced), and the `.pub` file goes last, so a reader that
// finds it finds a complete identity.
auto store_local_identity(const std::filesystem::path& directory,
                          const actor& identity,
                          std::span<const std::byte> private_material,
                          std::span<const std::byte> full_document) -> result<void> {
    auto wrapping_key = identity_wrapping_key(directory);
    if (!wrapping_key) {
        return std::unexpected(wrapping_key.error());
    }
    auto sealed = crypto_detail::seal(*wrapping_key, {}, private_material);
    crypto_wipe(wrapping_key->data(), wrapping_key->size());
    if (!sealed) {
        return std::unexpected(sealed.error());
    }
    auto written = write_private(directory / (identity.fingerprint + ".key"), *sealed);
    crypto_wipe(sealed->data(), sealed->size());
    if (!written) {
        return written;
    }
    std::error_code code;
    std::filesystem::create_directories(document_directory(identity), code);
    if (code) {
        return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    written = write_private(document_path(document_directory(identity), 0), full_document);
    if (!written) {
        return written;
    }
    written = write_private(directory / (identity.fingerprint + ".identity.cbor"), full_document);
    if (!written) {
        return written;
    }
    return write_private(directory / (identity.fingerprint + ".pub"), identity.public_key);
}

} // namespace

// Opens the local identity, creating it on first use. Concurrent first uses, in threads or in
// separate processes, all end with the same identity (liquid#349): the identity is read and created
// under the directory's lock, and a caller that finds one already created uses it and discards the
// key it made. The new identity's document is signed before the lock is taken, because signing may
// suspend.
runtime::task<signer> identity_detail::local_signer() {
    const auto make_signer = [](local_identity value) -> signer {
        signer output;
        output.impl_ = std::make_unique<signer::impl>();
        output.impl_->identity = std::move(value.identity);
        output.impl_->document = std::move(value.document);
        output.impl_->label = std::move(value.label);
        output.impl_->backend = std::move(value.backend);
        return output;
    };
    const auto directory = liquid_home() / "identity";
    std::error_code code;
    std::filesystem::create_directories(directory, code);
    if (code) {
        co_return std::unexpected(identity_error(errc::io, code.message(), directory.string()));
    }
    auto existing = load_local_identity_locked(directory);
    if (!existing) {
        co_return std::unexpected(existing.error());
    }
    if (*existing) {
        co_return make_signer(std::move(**existing));
    }

    auto label = std::string{"liquid-device"};
    auto created_at = identity_detail::now();
    auto made = identity_detail::make_software_backend();
    if (!made) {
        co_return std::unexpected(made.error());
    }
    auto backend = std::move(*made);
    auto derived = identity_detail::identifier(
        backend->public_key(), created_at, label, backend->algorithm(), backend->custody());
    if (!derived) {
        co_return std::unexpected(derived.error());
    }
    auto identity = std::move(*derived);
    identity_document document{identity, label, created_at, created_at, std::nullopt, {}, {}};
    auto body = identity_detail::canonical_document(document, false);
    if (!body) {
        co_return std::unexpected(body.error());
    }
    auto signed_bytes =
        co_await backend->sign(identity_detail::signing_input("identity_document", *body));
    if (!signed_bytes) {
        co_return std::unexpected(signed_bytes.error());
    }
    document.signatures.push_back({std::move(signed_bytes->algorithm),
                                   signing_key_identifier(identity),
                                   label,
                                   "identity_document",
                                   identity_detail::now(),
                                   std::move(signed_bytes->bytes),
                                   std::move(signed_bytes->authenticator_data),
                                   std::move(signed_bytes->client_data_hash)});
    auto full_document = identity_detail::canonical_document(document, true);
    if (!full_document) {
        co_return std::unexpected(full_document.error());
    }

    auto lock = identity_lock::acquire(directory);
    if (!lock) {
        co_return std::unexpected(lock.error());
    }
    existing = load_local_identity(directory);
    if (!existing) {
        co_return std::unexpected(existing.error());
    }
    if (*existing) {
        lock->release();
        co_return make_signer(std::move(**existing));
    }
    auto private_material = backend->descriptor();
    auto stored = store_local_identity(directory, identity, private_material, *full_document);
    crypto_wipe(private_material.data(), private_material.size());
    lock->release();
    if (!stored) {
        co_return std::unexpected(stored.error());
    }
    co_return make_signer(
        {std::move(identity), std::move(document), std::move(label), std::move(backend)});
}

struct passphrase_provider::impl {
    explicit impl(std::string value) : passphrase(std::move(value)) {}
    ~impl() {
        crypto_wipe(passphrase.data(), passphrase.size());
    }
    std::string passphrase;
};

passphrase_provider::passphrase_provider(std::string passphrase)
    : impl_(std::make_unique<impl>(std::move(passphrase))) {}
passphrase_provider::~passphrase_provider() = default;
passphrase_provider::passphrase_provider(passphrase_provider&&) noexcept = default;
passphrase_provider& passphrase_provider::operator=(passphrase_provider&&) noexcept = default;

runtime::task<key> key_provider::unwrap_legacy(std::span<const std::byte>,
                                               std::span<const std::byte>,
                                               argon2_parameters,
                                               wrap_context context) {
    co_return std::unexpected(key_error(errc::unauthorized,
                                        "custody provider cannot read a v1 or v2 wrapped key",
                                        custody_detail::reference(context)));
}

std::vector<std::byte> key_provider::parameters() const {
    return {};
}

result<key> passphrase_provider::derive(std::span<const std::byte> salt,
                                        argon2_parameters parameters) const {
    auto valid = validate_argon2(parameters);
    if (!valid)
        return std::unexpected(valid.error());
    if (salt.size() < 8 || salt.size() > std::numeric_limits<std::uint32_t>::max() ||
        impl_->passphrase.size() > std::numeric_limits<std::uint32_t>::max()) {
        return std::unexpected(key_error(
            errc::invalid_argument, "invalid passphrase or salt length", "passphrase_provider"));
    }
    std::vector<std::byte> work(static_cast<std::size_t>(parameters.memory_kib) * 1024U);
    key output{};
    const crypto_argon2_config config{
        CRYPTO_ARGON2_I, parameters.memory_kib, parameters.iterations, parameters.lanes};
    const crypto_argon2_inputs input{
        reinterpret_cast<const std::uint8_t*>(impl_->passphrase.data()),
        reinterpret_cast<const std::uint8_t*>(salt.data()),
        static_cast<std::uint32_t>(impl_->passphrase.size()),
        static_cast<std::uint32_t>(salt.size())};
    crypto_argon2(reinterpret_cast<std::uint8_t*>(output.data()),
                  output.size(),
                  work.data(),
                  config,
                  input,
                  crypto_argon2_no_extras);
    return output;
}

runtime::task<std::vector<std::byte>>
passphrase_provider::wrap(std::span<const std::byte, 32> data_key, wrap_context context) {
    auto salt = std::array<std::byte, 16>{};
    auto random = crypto_detail::random_bytes(salt);
    if (!random)
        co_return std::unexpected(random.error());
    const auto parameters = argon2_parameters{};
    auto kek = derive(salt, parameters);
    if (!kek)
        co_return std::unexpected(kek.error());
    auto wrapping = crypto_detail::derive_key(*kek, key_wrap_context);
    crypto_detail::wipe(*kek);
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto boxed = crypto_detail::seal(
        *wrapping, custody_detail::as_bytes(custody_detail::reference(context)), data_key);
    crypto_detail::wipe(*wrapping);
    if (!boxed)
        co_return std::unexpected(boxed.error());
    auto output = std::vector<std::byte>{salt.begin(), salt.end()};
    put_u32(output, parameters.memory_kib);
    put_u32(output, parameters.iterations);
    put_u32(output, parameters.lanes);
    output.insert(output.end(), boxed->begin(), boxed->end());
    co_return output;
}

runtime::task<key> passphrase_provider::unwrap(std::span<const std::byte> wrapped,
                                               wrap_context context) {
    if (wrapped.size() != 16 + 12 + boxed_key_size) {
        co_return std::unexpected(key_error(errc::unauthorized,
                                            "cannot unwrap passphrase data key",
                                            custody_detail::reference(context)));
    }
    const auto parameters =
        argon2_parameters{get_u32(wrapped, 16), get_u32(wrapped, 20), get_u32(wrapped, 24)};
    auto valid = validate_argon2(parameters);
    if (!valid)
        co_return std::unexpected(valid.error());
    auto kek = derive(wrapped.first<16>(), parameters);
    if (!kek)
        co_return std::unexpected(kek.error());
    auto wrapping = crypto_detail::derive_key(*kek, key_wrap_context);
    crypto_detail::wipe(*kek);
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto opened = crypto_detail::open(*wrapping,
                                      custody_detail::as_bytes(custody_detail::reference(context)),
                                      wrapped.subspan(28));
    crypto_detail::wipe(*wrapping);
    if (!opened || opened->size() != key{}.size()) {
        if (opened)
            crypto_detail::wipe(*opened);
        co_return std::unexpected(key_error(errc::unauthorized,
                                            "cannot unwrap passphrase data key",
                                            custody_detail::reference(context)));
    }
    auto output = key{};
    std::ranges::copy(*opened, output.begin());
    crypto_detail::wipe(*opened);
    co_return output;
}

std::string_view passphrase_provider::custody_kind() const noexcept {
    return "passphrase";
}

std::vector<std::byte> passphrase_provider::parameters() const {
    return {};
}

struct key_file_provider::impl {
    impl(key value, std::filesystem::path source) : key_bytes(value), path(std::move(source)) {}
    ~impl() {
        crypto_wipe(key_bytes.data(), key_bytes.size());
    }
    key key_bytes;
    std::filesystem::path path;
};

key_file_provider::key_file_provider(key key_bytes, std::filesystem::path path) noexcept
    : impl_(std::make_unique<impl>(key_bytes, std::move(path))) {}
key_file_provider::~key_file_provider() = default;
key_file_provider::key_file_provider(key_file_provider&&) noexcept = default;
key_file_provider& key_file_provider::operator=(key_file_provider&&) noexcept = default;

runtime::task<std::shared_ptr<key_file_provider>>
key_file_provider::open(std::filesystem::path path) {
    try {
        if (!std::filesystem::is_regular_file(path)) {
            co_return std::unexpected(
                key_error(errc::unauthorized, "key file is not a regular file", path.string()));
        }
#if !defined(_WIN32)
        struct stat status{};
        if (stat(path.c_str(), &status) != 0) {
            co_return std::unexpected(key_error(errc::io, "cannot stat key file", path.string()));
        }
        const auto mode = static_cast<unsigned>(status.st_mode & 0777U);
        if (mode != 0600U) {
            std::ostringstream context;
            context << path.string() << " mode " << std::oct << mode;
            co_return std::unexpected(
                key_error(errc::unauthorized, "key file must have mode 0600", context.str()));
        }
#endif
        std::ifstream input(path, std::ios::binary);
        key key_bytes{};
        input.read(reinterpret_cast<char*>(key_bytes.data()),
                   static_cast<std::streamsize>(key_bytes.size()));
        if (input.gcount() != static_cast<std::streamsize>(key_bytes.size()) ||
            input.peek() != EOF) {
            co_return std::unexpected(key_error(
                errc::invalid_argument, "key file must contain exactly 32 bytes", path.string()));
        }
        co_return std::shared_ptr<key_file_provider>(
            new key_file_provider(key_bytes, std::move(path)));
    } catch (const std::exception& caught) {
        co_return std::unexpected(key_error(errc::io, caught.what(), path.string()));
    }
}

runtime::task<std::vector<std::byte>>
key_file_provider::wrap(std::span<const std::byte, 32> data_key, wrap_context context) {
    auto wrapping = crypto_detail::derive_key(impl_->key_bytes, key_wrap_context);
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto output = crypto_detail::seal(
        *wrapping, custody_detail::as_bytes(custody_detail::reference(context)), data_key);
    crypto_detail::wipe(*wrapping);
    co_return output;
}

runtime::task<key> key_file_provider::unwrap(std::span<const std::byte> wrapped,
                                             wrap_context context) {
    auto wrapping = crypto_detail::derive_key(impl_->key_bytes, key_wrap_context);
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto opened = crypto_detail::open(
        *wrapping, custody_detail::as_bytes(custody_detail::reference(context)), wrapped);
    crypto_detail::wipe(*wrapping);
    if (!opened || opened->size() != key{}.size()) {
        if (opened)
            crypto_detail::wipe(*opened);
        co_return std::unexpected(key_error(errc::unauthorized,
                                            "cannot unwrap key-file data key",
                                            custody_detail::reference(context)));
    }
    auto output = key{};
    std::ranges::copy(*opened, output.begin());
    crypto_detail::wipe(*opened);
    co_return output;
}

std::string_view key_file_provider::custody_kind() const noexcept {
    return "file";
}

std::vector<std::byte> key_file_provider::parameters() const {
    return custody_detail::bytes(impl_->path.string());
}

namespace {

result<crypto_detail::key> derive_wrapping_key(result<crypto_detail::key> kek) {
    if (!kek)
        return std::unexpected(kek.error());
    auto wrapping = crypto_detail::derive_key(*kek, key_wrap_context);
    crypto_detail::wipe(*kek);
    return wrapping;
}

result<crypto_detail::key> unwrap_key(const crypto_detail::key& wrapping,
                                      std::span<const std::byte> boxed,
                                      std::string_view reference) {
    auto opened = crypto_detail::open(wrapping, {}, boxed);
    if (!opened || opened->size() != crypto_detail::key{}.size()) {
        if (opened)
            crypto_detail::wipe(*opened);
        return std::unexpected(
            key_error(errc::unauthorized, "cannot unwrap data key", std::string{reference}));
    }
    crypto_detail::key output{};
    std::copy_n(opened->begin(), output.size(), output.begin());
    crypto_detail::wipe(*opened);
    return output;
}

} // namespace

struct keyring::impl {
    std::map<std::string, std::shared_ptr<key_provider>, std::less<>> providers;
    std::string default_key;
    std::filesystem::path keys_path;
};

keyring::keyring() : impl_(std::make_unique<impl>()) {}
keyring::~keyring() = default;
keyring::keyring(keyring&&) noexcept = default;
keyring& keyring::operator=(keyring&&) noexcept = default;

result<void> keyring::add(std::string key_id, std::shared_ptr<key_provider> provider) {
    if (!valid_key_id(key_id))
        return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    if (!provider)
        return std::unexpected(key_error(errc::invalid_argument, "key provider is null", key_id));
    impl_->providers[std::move(key_id)] = std::move(provider);
    return {};
}

result<std::string> keyring::default_key_id() const {
    if (impl_->default_key.empty())
        return std::unexpected(
            key_error(errc::invalid_argument, "no default key is configured", "keyring"));
    return impl_->default_key;
}

result<void> keyring::set_default_key_id(std::string key_id) {
    if (!impl_->providers.contains(key_id))
        return std::unexpected(
            key_error(errc::unauthorized, "key id is not in the keyring", std::move(key_id)));
    impl_->default_key = std::move(key_id);
    return {};
}

runtime::task<void> keyring::attach(std::filesystem::path keys_path) {
    try {
        std::filesystem::create_directories(keys_path);
        impl_->keys_path = std::move(keys_path);
        co_return result<void>{};
    } catch (const std::exception& caught) {
        co_return std::unexpected(key_error(errc::io, caught.what(), keys_path.string()));
    }
}

runtime::task<key> passphrase_provider::unwrap_legacy(std::span<const std::byte> wrapped,
                                                      std::span<const std::byte> salt,
                                                      argon2_parameters parameters,
                                                      wrap_context context) {
    auto wrapping = derive_wrapping_key(derive(salt, parameters));
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto data = unwrap_key(*wrapping, wrapped, custody_detail::reference(context));
    crypto_detail::wipe(*wrapping);
    if (!data)
        co_return std::unexpected(data.error());
    co_return *data;
}

runtime::task<key> key_file_provider::unwrap_legacy(std::span<const std::byte> wrapped,
                                                    std::span<const std::byte>,
                                                    argon2_parameters,
                                                    wrap_context context) {
    auto wrapping = derive_wrapping_key(impl_->key_bytes);
    if (!wrapping)
        co_return std::unexpected(wrapping.error());
    auto data = unwrap_key(*wrapping, wrapped, custody_detail::reference(context));
    crypto_detail::wipe(*wrapping);
    if (!data)
        co_return std::unexpected(data.error());
    co_return *data;
}

runtime::task<std::string> keyring::write_key(std::string key_id) {
    if (!valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    if (!impl_->providers.contains(key_id)) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    if (!std::filesystem::exists(path)) {
        const auto reference = identity_detail::versioned_key_id(key_id, 1);
        auto data = co_await data_key(reference, true);
        if (!data)
            co_return std::unexpected(data.error());
        crypto_detail::wipe(*data);
        co_return reference;
    }
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    const auto reference = identity_detail::versioned_key_id(key_id, file->current_version);
    if (!file->versions.contains(file->current_version)) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "write key version is unavailable", reference));
    }
    co_return reference;
}

runtime::task<key> keyring::data_key(std::string key_id, bool create_if_missing) {
    auto parsed = identity_detail::parse_key_reference(key_id);
    if (!parsed)
        co_return std::unexpected(parsed.error());
    if (!valid_key_id(parsed->key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    const auto provider = impl_->providers.find(parsed->key_id);
    if (provider == impl_->providers.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto version = parsed->version.value_or(1);
    const auto reference = identity_detail::versioned_key_id(parsed->key_id, version);
    const auto path = impl_->keys_path / (parsed->key_id + ".wrapped");
    if (!std::filesystem::exists(path)) {
        if (!create_if_missing || version != 1) {
            co_return std::unexpected(
                key_error(errc::unauthorized, "wrapped data key is unavailable", reference));
        }
        auto data = crypto_detail::random_key();
        if (!data)
            co_return std::unexpected(data.error());
        const auto context = wrap_context{parsed->key_id, 1};
        auto boxed = co_await provider->second->wrap(*data, context);
        if (!boxed) {
            crypto_detail::wipe(*data);
            co_return std::unexpected(boxed.error());
        }
        wrapped_key_file file;
        file.versions.emplace(1,
                              wrapped_key_entry{std::string{provider->second->custody_kind()},
                                                provider->second->parameters(),
                                                std::move(*boxed),
                                                false});
        auto written = co_await write_atomic(path, encode_wrapped_key_file(file));
        if (!written) {
            crypto_detail::wipe(*data);
            co_return std::unexpected(written.error());
        }
        auto output = *data;
        crypto_detail::wipe(*data);
        co_return output;
    }
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, parsed->key_id);
    if (!file)
        co_return std::unexpected(file.error());
    const auto entry = file->versions.find(version);
    if (entry == file->versions.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "data key version is unavailable", reference));
    }
    auto data = result<key>{std::unexpected(key_error(errc::internal, "unreachable", reference))};
    if (entry->second.legacy) {
        data = co_await provider->second->unwrap_legacy(
            entry->second.wrapped, file->salt, file->parameters, {parsed->key_id, version});
    } else if (entry->second.custody != provider->second->custody_kind()) {
        data = std::unexpected(
            key_error(errc::unauthorized, "custody provider kind does not match", reference));
    } else {
        data = co_await provider->second->unwrap(entry->second.wrapped, {parsed->key_id, version});
    }
    if (!data)
        co_return std::unexpected(data.error());
    auto output = *data;
    crypto_detail::wipe(*data);
    co_return output;
}

runtime::task<void> keyring::rewrap(std::string key_id,
                                    std::shared_ptr<key_provider> new_provider) {
    if (!new_provider) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "key provider is null", key_id));
    }
    if (!valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    const auto provider = impl_->providers.find(key_id);
    if (provider == impl_->providers.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    if (file->versions.empty()) {
        co_return std::unexpected(key_error(
            errc::unauthorized, "no held data key version can authenticate the KEK", key_id));
    }
    auto replacement = *file;
    replacement.format = 3;
    replacement.versions.clear();
    for (const auto& [version, entry] : file->versions) {
        const auto reference = identity_detail::versioned_key_id(key_id, version);
        auto data =
            result<key>{std::unexpected(key_error(errc::internal, "unreachable", reference))};
        if (entry.legacy) {
            data = co_await provider->second->unwrap_legacy(
                entry.wrapped, file->salt, file->parameters, {key_id, version});
        } else if (entry.custody != provider->second->custody_kind()) {
            data = std::unexpected(
                key_error(errc::unauthorized, "custody provider kind does not match", reference));
        } else {
            data = co_await provider->second->unwrap(entry.wrapped, {key_id, version});
        }
        if (!data) {
            co_return std::unexpected(data.error());
        }
        auto reboxed = co_await new_provider->wrap(*data, {key_id, version});
        crypto_detail::wipe(*data);
        if (!reboxed) {
            co_return std::unexpected(reboxed.error());
        }
        replacement.versions.emplace(version,
                                     wrapped_key_entry{std::string{new_provider->custody_kind()},
                                                       new_provider->parameters(),
                                                       std::move(*reboxed),
                                                       false});
    }
    auto written = co_await write_atomic(path, encode_wrapped_key_file(replacement));
    if (!written)
        co_return std::unexpected(written.error());
    impl_->providers[std::move(key_id)] = std::move(new_provider);
    co_return result<void>{};
}

runtime::task<rotation_report> keyring::rotate(std::string key_id) {
    if (!valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    const auto provider = impl_->providers.find(key_id);
    if (provider == impl_->providers.end()) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    if (file->versions.empty()) {
        co_return std::unexpected(key_error(
            errc::unauthorized, "no held data key version can authenticate the KEK", key_id));
    }
    auto highest = file->current_version;
    if (!file->versions.empty())
        highest = std::max(highest, file->versions.rbegin()->first);
    if (!file->revoked.empty())
        highest = std::max(highest, *file->revoked.rbegin());
    if (highest == std::numeric_limits<std::uint64_t>::max()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "key version space is exhausted", key_id));
    }
    auto replacement = *file;
    replacement.format = 3;
    replacement.versions.clear();
    for (const auto& [version, entry] : file->versions) {
        const auto reference = identity_detail::versioned_key_id(key_id, version);
        auto data =
            result<key>{std::unexpected(key_error(errc::internal, "unreachable", reference))};
        if (entry.legacy) {
            data = co_await provider->second->unwrap_legacy(
                entry.wrapped, file->salt, file->parameters, {key_id, version});
        } else if (entry.custody != provider->second->custody_kind()) {
            data = std::unexpected(
                key_error(errc::unauthorized, "custody provider kind does not match", reference));
        } else {
            data = co_await provider->second->unwrap(entry.wrapped, {key_id, version});
        }
        if (!data)
            co_return std::unexpected(data.error());
        auto boxed = co_await provider->second->wrap(*data, {key_id, version});
        crypto_detail::wipe(*data);
        if (!boxed)
            co_return std::unexpected(boxed.error());
        replacement.versions.emplace(
            version,
            wrapped_key_entry{std::string{provider->second->custody_kind()},
                              provider->second->parameters(),
                              std::move(*boxed),
                              false});
    }
    auto data = crypto_detail::random_key();
    if (!data)
        co_return std::unexpected(data.error());
    const auto previous = file->current_version;
    const auto next = highest + 1;
    auto boxed = co_await provider->second->wrap(*data, {key_id, next});
    crypto_detail::wipe(*data);
    if (!boxed)
        co_return std::unexpected(boxed.error());
    replacement.current_version = next;
    replacement.versions.emplace(next,
                                 wrapped_key_entry{std::string{provider->second->custody_kind()},
                                                   provider->second->parameters(),
                                                   std::move(*boxed),
                                                   false});
    auto written = co_await write_atomic(path, encode_wrapped_key_file(replacement));
    if (!written)
        co_return std::unexpected(written.error());
    co_return rotation_report{std::move(key_id), previous, next, encryption_limits()};
}

runtime::task<void> keyring::revoke(std::string key_id, std::uint64_t version) {
    if (!valid_key_id(key_id) || version == 0) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id or version", std::move(key_id)));
    }
    if (!impl_->providers.contains(key_id)) {
        co_return std::unexpected(
            key_error(errc::unauthorized, "key is unavailable", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    const auto path = impl_->keys_path / (key_id + ".wrapped");
    auto raw = co_await store_detail::read_file(path);
    if (!raw)
        co_return std::unexpected(raw.error());
    auto file = parse_wrapped_key_file(*raw, key_id);
    if (!file)
        co_return std::unexpected(file.error());
    if (file->revoked.contains(version))
        co_return result<void>{};
    if (file->versions.erase(version) == 0) {
        const auto reference = identity_detail::versioned_key_id(key_id, version);
        co_return std::unexpected(
            key_error(errc::not_found, "data key version does not exist", reference));
    }
    file->revoked.insert(version);
    auto encoded =
        file->format < 3 ? encode_wrapped_key_file_v2(*file) : encode_wrapped_key_file(*file);
    auto written = co_await write_atomic(path, std::move(encoded));
    if (!written)
        co_return std::unexpected(written.error());
    co_return result<void>{};
}

runtime::task<std::vector<custody_record>> keyring::show(std::string key_id) {
    if (!key_id.empty() && !valid_key_id(key_id)) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "invalid key id", std::move(key_id)));
    }
    if (impl_->keys_path.empty()) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "keyring is not attached to a store", key_id));
    }
    auto paths = std::vector<std::filesystem::path>{};
    if (!key_id.empty()) {
        paths.push_back(impl_->keys_path / (key_id + ".wrapped"));
    } else {
        std::error_code code;
        for (const auto& entry : std::filesystem::directory_iterator(impl_->keys_path, code)) {
            if (entry.is_regular_file() && entry.path().extension() == ".wrapped")
                paths.push_back(entry.path());
        }
        if (code)
            co_return std::unexpected(
                key_error(errc::io, code.message(), impl_->keys_path.string()));
        std::ranges::sort(paths);
    }
    auto output = std::vector<custody_record>{};
    for (const auto& path : paths) {
        auto raw = co_await store_detail::read_file(path);
        if (!raw)
            co_return std::unexpected(raw.error());
        const auto id = path.stem().string();
        auto file = parse_wrapped_key_file(*raw, id);
        if (!file)
            co_return std::unexpected(file.error());
        for (const auto& [version, entry] : file->versions) {
            output.push_back({id,
                              version,
                              entry.legacy ? "legacy" : entry.custody,
                              entry.parameters,
                              version == file->current_version,
                              false});
        }
        for (const auto version : file->revoked) {
            output.push_back({id, version, "revoked", {}, version == file->current_version, true});
        }
    }
    co_return output;
}

runtime::task<std::shared_ptr<key_provider>> key_provider_from_uri(std::string custody_uri) {
    const auto separator = custody_uri.find(':');
    if (separator == std::string::npos) {
        co_return std::unexpected(
            key_error(errc::invalid_argument, "custody URI has no scheme", custody_uri));
    }
    const auto kind = custody_uri.substr(0, separator);
    auto parameters = custody_uri.substr(separator + 1);
    if (kind == "passphrase") {
        if (!parameters.empty()) {
            co_return std::unexpected(key_error(errc::invalid_argument,
                                                "passphrase custody URI must not contain a secret",
                                                "passphrase:"));
        }
        auto passphrase = custody_detail::environment("LIQUID_PASSPHRASE");
        if (!passphrase)
            co_return std::unexpected(passphrase.error());
        co_return std::make_shared<passphrase_provider>(std::move(*passphrase));
    }
    if (kind == "file")
        co_return co_await key_file_provider::open(std::move(parameters));
    auto provider = result<std::shared_ptr<key_provider>>{
        std::unexpected(key_error(errc::invalid_argument, "unsupported custody URI", custody_uri))};
    if (kind == "age")
        provider = custody_detail::make_age_provider(std::move(parameters));
    else if (kind == "aws-kms")
        provider = custody_detail::make_aws_kms_provider(std::move(parameters));
    else if (kind == "gcp-kms")
        provider = custody_detail::make_gcp_kms_provider(std::move(parameters));
    else if (kind == "azure-kv")
        provider = custody_detail::make_azure_kv_provider(std::move(parameters));
    else if (kind == "lake")
        provider = custody_detail::make_lake_provider(std::move(parameters));
    else if (kind == "shamir")
        provider = custody_detail::make_shamir_provider(std::move(parameters));
    else if (kind == "tpm")
        provider = custody_detail::make_tpm_provider(std::move(parameters));
    if (!provider)
        co_return std::unexpected(provider.error());
    co_return *provider;
}

result<custody_split> split_custody(std::uint8_t threshold, std::uint8_t shares) {
    return custody_detail::make_shamir_split(threshold, shares);
}

runtime::task<std::shared_ptr<keyring>> keyring::open(std::filesystem::path keys_path) {
    auto output = std::make_shared<keyring>();
    auto attached = co_await output->attach(std::move(keys_path));
    if (!attached)
        co_return std::unexpected(attached.error());
    auto records = co_await output->show();
    if (!records)
        co_return std::unexpected(records.error());
    auto configured = std::set<std::string>{};
    for (const auto& record : *records) {
        if (record.revoked || !configured.insert(record.key_id).second)
            continue;
        auto uri = record.custody == "legacy"
                       ? std::string{"passphrase:"}
                       : record.custody + ":" + custody_detail::text(record.parameters);
        auto provider = co_await key_provider_from_uri(std::move(uri));
        if (!provider)
            continue;
        auto added = output->add(record.key_id, std::move(*provider));
        if (!added)
            co_return std::unexpected(added.error());
        if (output->impl_->default_key.empty())
            output->impl_->default_key = record.key_id;
    }
    co_return output;
}

} // namespace liquid
