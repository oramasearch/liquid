#include "reapi/action_cache.hpp"

#include <algorithm>
#include <cerrno>
#include <cstring>
#include <random>
#include <utility>

#if defined(_WIN32)
#include <fstream>
#include <iterator>
#else
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace liquid::reapi {
namespace {

auto cache_error(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto digest_length(algorithm hash_algorithm) -> result<std::size_t> {
    switch (hash_algorithm) {
    case algorithm::blake3:
    case algorithm::sha256:
        return std::size_t{32};
    default:
        return std::unexpected(
            cache_error(errc::invalid_argument, "action digest algorithm is unsupported"));
    }
}

auto lowercase_hex(std::string_view value) -> bool {
    return !value.empty() && std::ranges::all_of(value, [](char character) {
        return (character >= '0' && character <= '9') || (character >= 'a' && character <= 'f');
    });
}

// The entry-level check repeated by every file operation, so no caller can pass a path.
auto valid_name(std::string_view name) -> result<void> {
    if (!lowercase_hex(name) || name.size() > 128) {
        return std::unexpected(
            cache_error(errc::invalid_argument, "action cache entry name is invalid"));
    }
    return {};
}

auto temporary_name(std::string_view name) -> std::string {
    auto random = std::random_device{};
    auto distribution = std::uniform_int_distribution<std::uint64_t>{};
    return ".tmp-" + std::string{name} + "-" + std::to_string(distribution(random));
}

#if !defined(_WIN32)

auto system_error(int code, std::string message, std::string context) -> error {
    return cache_error(
        errc::io, std::move(message) + ": " + std::strerror(code), std::move(context));
}

class descriptor {
public:
    explicit descriptor(int value) noexcept : value_(value) {}
    ~descriptor() {
        if (value_ >= 0) {
            ::close(value_);
        }
    }
    descriptor(const descriptor&) = delete;
    auto operator=(const descriptor&) -> descriptor& = delete;

    [[nodiscard]] auto get() const noexcept -> int {
        return value_;
    }
    [[nodiscard]] auto release() noexcept -> int {
        return std::exchange(value_, -1);
    }

private:
    int value_;
};

auto open_root(const std::filesystem::path& root) -> result<int> {
    const auto value = ::open(root.c_str(), O_RDONLY | O_DIRECTORY | O_NOFOLLOW | O_CLOEXEC);
    if (value < 0) {
        return std::unexpected(
            system_error(errno, "action cache directory cannot be opened", root.string()));
    }
    return value;
}

#endif

} // namespace

auto action_cache_name(const Digest& action, algorithm hash_algorithm) -> result<std::string> {
    auto length = digest_length(hash_algorithm);
    if (!length) {
        return std::unexpected(length.error());
    }
    if (action.size_bytes < 0 || action.hash.size() != *length * 2 || !lowercase_hex(action.hash)) {
        return std::unexpected(cache_error(errc::invalid_argument,
                                           "action digest is not a lowercase hexadecimal " +
                                               std::to_string(*length * 8) + "-bit digest",
                                           action.hash.substr(0, 128)));
    }
    return action.hash;
}

#if !defined(_WIN32)

auto read_action_cache(const std::filesystem::path& root, std::string_view name)
    -> result<std::vector<std::byte>> {
    auto valid = valid_name(name);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    auto opened = open_root(root);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    const auto directory = descriptor{*opened};
    const auto entry = std::string{name};
    auto file = descriptor{
        ::openat(directory.get(), entry.c_str(), O_RDONLY | O_NOFOLLOW | O_NONBLOCK | O_CLOEXEC)};
    if (file.get() < 0) {
        const auto code = errno;
        if (code == ENOENT) {
            return std::unexpected(
                cache_error(errc::not_found, "action result is not cached", entry));
        }
        if (code == ELOOP) {
            return std::unexpected(
                cache_error(errc::corrupt, "action cache entry is a symbolic link", entry));
        }
        return std::unexpected(system_error(code, "action cache entry cannot be opened", entry));
    }
    struct stat status{};
    if (::fstat(file.get(), &status) != 0) {
        return std::unexpected(system_error(errno, "action cache entry cannot be read", entry));
    }
    if (!S_ISREG(status.st_mode)) {
        return std::unexpected(
            cache_error(errc::corrupt, "action cache entry is not a regular file", entry));
    }
    auto output = std::vector<std::byte>(static_cast<std::size_t>(status.st_size));
    auto offset = std::size_t{};
    while (offset < output.size()) {
        const auto count = ::read(file.get(), output.data() + offset, output.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count < 0) {
            return std::unexpected(system_error(errno, "action cache entry cannot be read", entry));
        }
        if (count == 0) {
            output.resize(offset);
            break;
        }
        offset += static_cast<std::size_t>(count);
    }
    return output;
}

auto write_action_cache(const std::filesystem::path& root,
                        std::string_view name,
                        std::span<const std::byte> contents) -> result<void> {
    auto valid = valid_name(name);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    auto opened = open_root(root);
    if (!opened) {
        return std::unexpected(opened.error());
    }
    const auto directory = descriptor{*opened};
    const auto entry = std::string{name};
    const auto temporary = temporary_name(name);
    auto file = descriptor{::openat(directory.get(),
                                    temporary.c_str(),
                                    O_WRONLY | O_CREAT | O_EXCL | O_NOFOLLOW | O_CLOEXEC,
                                    0644)};
    if (file.get() < 0) {
        return std::unexpected(system_error(errno, "action cache entry cannot be created", entry));
    }
    const auto discard = [&] { ::unlinkat(directory.get(), temporary.c_str(), 0); };
    auto offset = std::size_t{};
    while (offset < contents.size()) {
        const auto count = ::write(file.get(), contents.data() + offset, contents.size() - offset);
        if (count < 0 && errno == EINTR) {
            continue;
        }
        if (count <= 0) {
            const auto code = errno;
            discard();
            return std::unexpected(
                system_error(code, "action cache entry cannot be written", entry));
        }
        offset += static_cast<std::size_t>(count);
    }
    if (::close(file.release()) != 0) {
        const auto code = errno;
        discard();
        return std::unexpected(system_error(code, "action cache entry cannot be written", entry));
    }
    // rename replaces the directory entry itself: a symbolic link under this name is replaced,
    // and its target is never opened.
    if (::renameat(directory.get(), temporary.c_str(), directory.get(), entry.c_str()) != 0) {
        const auto code = errno;
        discard();
        return std::unexpected(system_error(code, "action cache entry cannot be replaced", entry));
    }
    return {};
}

#else

auto read_action_cache(const std::filesystem::path& root, std::string_view name)
    -> result<std::vector<std::byte>> {
    auto valid = valid_name(name);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    const auto path = root / std::string{name};
    auto code = std::error_code{};
    const auto status = std::filesystem::symlink_status(path, code);
    if (code || status.type() == std::filesystem::file_type::not_found) {
        return std::unexpected(
            cache_error(errc::not_found, "action result is not cached", std::string{name}));
    }
    if (status.type() != std::filesystem::file_type::regular) {
        return std::unexpected(cache_error(
            errc::corrupt, "action cache entry is not a regular file", std::string{name}));
    }
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(
            cache_error(errc::io, "action cache entry cannot be read", std::string{name}));
    }
    auto text = std::string{std::istreambuf_iterator<char>{input}, {}};
    const auto* first = reinterpret_cast<const std::byte*>(text.data());
    return std::vector<std::byte>{first, first + text.size()};
}

auto write_action_cache(const std::filesystem::path& root,
                        std::string_view name,
                        std::span<const std::byte> contents) -> result<void> {
    auto valid = valid_name(name);
    if (!valid) {
        return std::unexpected(valid.error());
    }
    const auto temporary = root / temporary_name(name);
    {
        auto output = std::ofstream{temporary, std::ios::binary | std::ios::trunc};
        output.write(reinterpret_cast<const char*>(contents.data()),
                     static_cast<std::streamsize>(contents.size()));
        if (!output) {
            return std::unexpected(
                cache_error(errc::io, "action cache entry cannot be written", std::string{name}));
        }
    }
    auto code = std::error_code{};
    std::filesystem::rename(temporary, root / std::string{name}, code);
    if (code) {
        std::filesystem::remove(temporary, code);
        return std::unexpected(
            cache_error(errc::io, "action cache entry cannot be replaced", std::string{name}));
    }
    return {};
}

#endif

} // namespace liquid::reapi
