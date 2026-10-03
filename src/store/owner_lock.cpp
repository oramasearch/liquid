#include "owner_lock.hpp"

#include <algorithm>
#include <array>
#include <cerrno>
#include <cstring>
#include <ctime>
#include <fstream>
#include <iterator>
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

namespace liquid::store_detail {
namespace {

constexpr auto first_retry = std::chrono::milliseconds{5};
constexpr auto longest_retry = std::chrono::milliseconds{100};

error lock_error(errc code, std::string message, const std::filesystem::path& path) {
    return {code, std::move(message), path.string()};
}

std::string utc_now() {
    const auto now = std::time(nullptr);
    std::tm parts{};
#if defined(_WIN32)
    gmtime_s(&parts, &now);
#else
    gmtime_r(&now, &parts);
#endif
    std::array<char, 32> text{};
    const auto length = std::strftime(text.data(), text.size(), "%Y-%m-%dT%H:%M:%SZ", &parts);
    return std::string(text.data(), length);
}

std::string host_name() {
#if defined(_WIN32)
    std::array<char, MAX_COMPUTERNAME_LENGTH + 1> name{};
    DWORD size = static_cast<DWORD>(name.size());
    if (GetComputerNameA(name.data(), &size) == 0)
        return "unknown";
    return std::string(name.data(), size);
#else
    std::array<char, 256> name{};
    if (::gethostname(name.data(), name.size() - 1) != 0)
        return "unknown";
    return std::string(name.data());
#endif
}

std::string owner_record() {
#if defined(_WIN32)
    const auto pid = static_cast<unsigned long>(GetCurrentProcessId());
#else
    const auto pid = static_cast<long>(::getpid());
#endif
    return "pid " + std::to_string(pid) + "\nhost " + host_name() + "\nsince " + utc_now() + "\n";
}

// Reads the holder's record for the refusal message. The holder rewrites it after locking, so a
// read can race a rewrite; the message is diagnostic only.
std::string describe_holder(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    std::string text{std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
    std::string pid;
    std::string host;
    std::string since;
    std::size_t start = 0;
    while (start < text.size()) {
        auto end = text.find('\n', start);
        if (end == std::string::npos)
            end = text.size();
        const auto line = std::string_view(text).substr(start, end - start);
        if (line.starts_with("pid "))
            pid = line.substr(4);
        else if (line.starts_with("host "))
            host = line.substr(5);
        else if (line.starts_with("since "))
            since = line.substr(6);
        start = end + 1;
    }
    if (pid.empty())
        return "another open store handle";
    return "pid " + pid + " on host " + host + " since " + since;
}

#if defined(_WIN32)
using native_handle = void*;
constexpr native_handle no_handle = nullptr;

void close_native(native_handle handle) noexcept {
    if (handle == no_handle)
        return;
    OVERLAPPED whole{};
    static_cast<void>(UnlockFileEx(static_cast<HANDLE>(handle), 0, MAXDWORD, MAXDWORD, &whole));
    static_cast<void>(CloseHandle(static_cast<HANDLE>(handle)));
}

// One attempt: the handle on success, std::nullopt when another holder has it.
result<std::optional<native_handle>> try_lock(const std::filesystem::path& path) {
    SECURITY_ATTRIBUTES attributes{};
    attributes.nLength = sizeof(attributes);
    attributes.bInheritHandle = FALSE;
    const auto handle = CreateFileW(path.c_str(),
                                    GENERIC_READ | GENERIC_WRITE,
                                    FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                                    &attributes,
                                    OPEN_ALWAYS,
                                    FILE_ATTRIBUTE_NORMAL,
                                    nullptr);
    if (handle == INVALID_HANDLE_VALUE) {
        return std::unexpected(lock_error(
            errc::io, std::system_category().message(static_cast<int>(GetLastError())), path));
    }
    OVERLAPPED whole{};
    if (LockFileEx(handle,
                   LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY,
                   0,
                   MAXDWORD,
                   MAXDWORD,
                   &whole) == 0) {
        const auto code = GetLastError();
        static_cast<void>(CloseHandle(handle));
        if (code == ERROR_LOCK_VIOLATION || code == ERROR_IO_PENDING)
            return std::optional<native_handle>{};
        return std::unexpected(
            lock_error(errc::io, std::system_category().message(static_cast<int>(code)), path));
    }
    return std::optional<native_handle>{handle};
}

void write_record(native_handle handle) noexcept {
    const auto record = owner_record();
    const auto file = static_cast<HANDLE>(handle);
    LARGE_INTEGER origin{};
    if (SetFilePointerEx(file, origin, nullptr, FILE_BEGIN) == 0)
        return;
    if (SetEndOfFile(file) == 0)
        return;
    DWORD written = 0;
    static_cast<void>(
        WriteFile(file, record.data(), static_cast<DWORD>(record.size()), &written, nullptr));
}
#else
using native_handle = int;
constexpr native_handle no_handle = -1;

void close_native(native_handle descriptor) noexcept {
    if (descriptor < 0)
        return;
    static_cast<void>(::flock(descriptor, LOCK_UN));
    static_cast<void>(::close(descriptor));
}

result<std::optional<native_handle>> try_lock(const std::filesystem::path& path) {
    const auto descriptor = ::open(path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (descriptor < 0)
        return std::unexpected(lock_error(errc::io, std::strerror(errno), path));
    while (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const auto code = errno;
        if (code == EINTR)
            continue;
        static_cast<void>(::close(descriptor));
        if (code == EWOULDBLOCK)
            return std::optional<native_handle>{};
        return std::unexpected(lock_error(errc::io, std::strerror(code), path));
    }
    return std::optional<native_handle>{descriptor};
}

void write_record(native_handle descriptor) noexcept {
    const auto record = owner_record();
    if (::ftruncate(descriptor, 0) != 0)
        return;
    static_cast<void>(::pwrite(descriptor, record.data(), record.size(), 0));
}
#endif

} // namespace

owner_lock::~owner_lock() {
    release();
}

owner_lock::owner_lock(owner_lock&& other) noexcept
#if defined(_WIN32)
    : handle_(std::exchange(other.handle_, nullptr)){}
#else
    : descriptor_(std::exchange(other.descriptor_, -1)) {
}
#endif

      owner_lock
      & owner_lock::operator=(owner_lock&& other) noexcept {
    if (this != &other) {
        release();
#if defined(_WIN32)
        handle_ = std::exchange(other.handle_, nullptr);
#else
        descriptor_ = std::exchange(other.descriptor_, -1);
#endif
    }
    return *this;
}

bool owner_lock::held() const noexcept {
#if defined(_WIN32)
    return handle_ != nullptr;
#else
    return descriptor_ >= 0;
#endif
}

void owner_lock::release() noexcept {
#if defined(_WIN32)
    close_native(std::exchange(handle_, nullptr));
#else
    close_native(std::exchange(descriptor_, -1));
#endif
}

runtime::task<owner_lock> owner_lock::acquire(std::filesystem::path path,
                                              std::chrono::milliseconds wait) {
    if (wait.count() < 0) {
        co_return std::unexpected(
            lock_error(errc::invalid_argument, "store owner wait must not be negative", path));
    }
    const auto deadline = std::chrono::steady_clock::now() + wait;
    auto retry = first_retry;
    for (;;) {
        auto attempt = co_await runtime::offload([path]() -> result<std::optional<native_handle>> {
            std::error_code created;
            std::filesystem::create_directories(path.parent_path(), created);
            if (created)
                return std::unexpected(lock_error(errc::io, created.message(), path));
            return try_lock(path);
        });
        if (!attempt)
            co_return std::unexpected(attempt.error());
        if (!*attempt)
            co_return std::unexpected(attempt->error());
        if (auto acquired = **attempt) {
            write_record(*acquired);
            owner_lock output;
#if defined(_WIN32)
            output.handle_ = *acquired;
#else
            output.descriptor_ = *acquired;
#endif
            co_return output;
        }
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) {
            auto holder = co_await runtime::offload([path] { return describe_holder(path); });
            co_return std::unexpected(lock_error(
                errc::locked,
                "store root is owned by " + (holder ? *holder : std::string{"another owner"}) +
                    "; one open store handle owns a root at a time",
                path));
        }
        const auto remaining =
            std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now);
        auto [delay_error] = co_await runtime::delay(std::min(retry, remaining));
        if (delay_error) {
            co_return std::unexpected(
                lock_error(errc::unavailable, "waiting for the store owner was cancelled", path));
        }
        retry = std::min(retry * 2, longest_retry);
    }
}

} // namespace liquid::store_detail
