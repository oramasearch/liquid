// SPDX-License-Identifier: MIT

#include "write_lock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <optional>
#include <string>
#include <system_error>
#include <utility>

#if defined(_WIN32)
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include "owner_lock.hpp"

namespace liquid::store_detail {
namespace {

constexpr auto first_retry = std::chrono::milliseconds{1};
constexpr auto longest_retry = std::chrono::milliseconds{50};
constexpr std::size_t generation_size = 8;
constexpr std::size_t holder_capacity = 248;

error write_lock_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}

std::array<std::byte, generation_size> encode_generation(std::uint64_t value) {
    std::array<std::byte, generation_size> bytes{};
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        bytes[index] = std::byte{static_cast<unsigned char>(value >> (8U * index))};
    }
    return bytes;
}

std::uint64_t decode_generation(const std::array<std::byte, generation_size>& bytes) {
    std::uint64_t value = 0;
    for (std::size_t index = 0; index != bytes.size(); ++index) {
        value |= std::uint64_t(std::to_integer<unsigned char>(bytes[index])) << (8U * index);
    }
    return value;
}

// The holder record: a fixed-size, NUL-padded field after the generation, so writing it never
// changes the file's length and a reader never sees a torn generation.
std::array<char, holder_capacity> holder_record() {
    std::array<char, holder_capacity> record{};
    const auto text = holder_description();
    std::copy_n(text.data(), std::min(text.size(), record.size() - 1), record.data());
    return record;
}

#if defined(_WIN32)
OVERLAPPED at(std::uint64_t offset) {
    OVERLAPPED position{};
    position.Offset = static_cast<DWORD>(offset & 0xffffffffULL);
    position.OffsetHigh = static_cast<DWORD>(offset >> 32U);
    return position;
}

bool read_at(void* handle, std::uint64_t offset, void* into, std::size_t size, std::size_t& read) {
    auto position = at(offset);
    DWORD count = 0;
    if (ReadFile(static_cast<HANDLE>(handle), into, static_cast<DWORD>(size), &count, &position) ==
        0) {
        if (GetLastError() == ERROR_HANDLE_EOF) {
            read = 0;
            return true;
        }
        return false;
    }
    read = count;
    return true;
}

bool write_at(void* handle, std::uint64_t offset, const void* from, std::size_t size) {
    auto position = at(offset);
    DWORD count = 0;
    return WriteFile(
               static_cast<HANDLE>(handle), from, static_cast<DWORD>(size), &count, &position) !=
               0 &&
           count == size;
}

std::string last_error() {
    return std::system_category().message(static_cast<int>(GetLastError()));
}
#else
bool read_at(
    int descriptor, std::uint64_t offset, void* into, std::size_t size, std::size_t& read) {
    for (;;) {
        const auto count = ::pread(descriptor, into, size, static_cast<off_t>(offset));
        if (count >= 0) {
            read = static_cast<std::size_t>(count);
            return true;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

bool write_at(int descriptor, std::uint64_t offset, const void* from, std::size_t size) {
    for (;;) {
        const auto count = ::pwrite(descriptor, from, size, static_cast<off_t>(offset));
        if (count >= 0) {
            return static_cast<std::size_t>(count) == size;
        }
        if (errno != EINTR) {
            return false;
        }
    }
}

std::string last_error() {
    return std::strerror(errno);
}
#endif

} // namespace

write_lock::~write_lock() {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        OVERLAPPED whole{};
        static_cast<void>(
            UnlockFileEx(static_cast<HANDLE>(handle_), 0, MAXDWORD, MAXDWORD, &whole));
        static_cast<void>(CloseHandle(static_cast<HANDLE>(handle_)));
    }
#else
    if (descriptor_ >= 0) {
        static_cast<void>(::flock(descriptor_, LOCK_UN));
        static_cast<void>(::close(descriptor_));
    }
#endif
}

write_lock::write_lock(write_lock&& other) noexcept
    : path_(std::move(other.path_))
#if defined(_WIN32)
      ,
      handle_(std::exchange(other.handle_, nullptr))
#else
      ,
      descriptor_(std::exchange(other.descriptor_, -1))
#endif
{
}

write_lock& write_lock::operator=(write_lock&& other) noexcept {
    if (this != &other) {
        write_lock released(std::move(*this));
        path_ = std::move(other.path_);
#if defined(_WIN32)
        handle_ = std::exchange(other.handle_, nullptr);
#else
        descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    }
    return *this;
}

result<write_lock> write_lock::open(std::filesystem::path path) {
    std::error_code created;
    std::filesystem::create_directories(path.parent_path(), created);
    if (created) {
        return std::unexpected(write_lock_error(errc::io, created.message(), path));
    }
    write_lock output;
    output.path_ = std::move(path);
#if defined(_WIN32)
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = FALSE;
    const auto handle = CreateFileW(output.path_.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    &attributes,
                                    OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(write_lock_error(errc::io, last_error(), output.path_));
    }
    output.handle_ = handle;
#else
    const auto descriptor = ::open(output.path_.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (descriptor < 0) {
        return std::unexpected(write_lock_error(errc::io, last_error(), output.path_));
    }
    output.descriptor_ = descriptor;
#endif
    return output;
}

bool write_lock::valid() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return descriptor_ >= 0;
#endif
}

runtime::task<void> write_lock::acquire(std::chrono::milliseconds wait) {
    if (!valid()) {
        co_return std::unexpected(
            write_lock_error(errc::invalid_argument, "the store write lock is not open", path_));
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    auto retry = first_retry;
    for (;;) {
        // One non-blocking attempt; the waiting happens on the runtime's timer, not in a thread.
#if defined(_WIN32)
        OVERLAPPED whole{};
        if (LockFileEx(static_cast<HANDLE>(handle_),
                       LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                       0,
                       MAXDWORD,
                       MAXDWORD,
                       &whole) != 0) {
            break;
        }
        const auto code = GetLastError();
        if (code != ERROR_LOCK_VIOLATION && code != ERROR_IO_PENDING) {
            co_return std::unexpected(write_lock_error(
                errc::io, std::system_category().message(static_cast<int>(code)), path_));
        }
#else
        if (::flock(descriptor_, LOCK_EX | LOCK_NB) == 0) {
            break;
        }
        const auto code = errno;
        if (code == EINTR) {
            continue;
        }
        if (code != EWOULDBLOCK) {
            co_return std::unexpected(write_lock_error(errc::io, std::strerror(code), path_));
        }
#endif
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            std::array<char, holder_capacity> holder{};
            auto read = std::size_t{};
#if defined(_WIN32)
            const auto native = handle_;
#else
            const auto native = descriptor_;
#endif
            auto described = read_at(native, generation_size, holder.data(), holder.size(), read);
            auto text = described && read != 0
                            ? std::string(holder.data(), strnlen(holder.data(), read))
                            : std::string{};
            if (text.empty()) {
                text = "another writer";
            }
            co_return std::unexpected(write_lock_error(
                errc::busy,
                "store is being written by " + text + "; its write lock was not released in time",
                path_));
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto [delay_error] = co_await runtime::delay(std::min(retry, remaining));
        if (delay_error) {
            co_return std::unexpected(write_lock_error(
                errc::unavailable, "waiting for the store write lock was cancelled", path_));
        }
        retry = std::min(retry * 2, longest_retry);
    }
    const auto record = holder_record();
#if defined(_WIN32)
    static_cast<void>(write_at(handle_, generation_size, record.data(), record.size()));
#else
    static_cast<void>(write_at(descriptor_, generation_size, record.data(), record.size()));
#endif
    co_return result<void>{};
}

void write_lock::release() noexcept {
#if defined(_WIN32)
    if (handle_ != nullptr) {
        OVERLAPPED whole{};
        static_cast<void>(
            UnlockFileEx(static_cast<HANDLE>(handle_), 0, MAXDWORD, MAXDWORD, &whole));
    }
#else
    if (descriptor_ >= 0) {
        static_cast<void>(::flock(descriptor_, LOCK_UN));
    }
#endif
}

result<std::uint64_t> write_lock::generation() const {
    if (!valid()) {
        return std::unexpected(
            write_lock_error(errc::invalid_argument, "the store write lock is not open", path_));
    }
    std::array<std::byte, generation_size> bytes{};
    auto read = std::size_t{};
#if defined(_WIN32)
    const auto ok = read_at(handle_, 0, bytes.data(), bytes.size(), read);
#else
    const auto ok = read_at(descriptor_, 0, bytes.data(), bytes.size(), read);
#endif
    if (!ok) {
        return std::unexpected(write_lock_error(errc::io, last_error(), path_));
    }
    if (read < bytes.size()) {
        return std::uint64_t{0};
    }
    return decode_generation(bytes);
}

result<void> write_lock::set_generation(std::uint64_t value) {
    if (!valid()) {
        return std::unexpected(
            write_lock_error(errc::invalid_argument, "the store write lock is not open", path_));
    }
    const auto bytes = encode_generation(value);
#if defined(_WIN32)
    const auto ok = write_at(handle_, 0, bytes.data(), bytes.size());
#else
    const auto ok = write_at(descriptor_, 0, bytes.data(), bytes.size());
#endif
    if (!ok) {
        return std::unexpected(write_lock_error(errc::io, last_error(), path_));
    }
    return {};
}

} // namespace liquid::store_detail
