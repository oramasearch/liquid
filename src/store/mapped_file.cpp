// SPDX-License-Identifier: MIT

#include "mapped_file.hpp"

#include "backend.hpp"

#include <system_error>
#include <utility>

#if defined(_WIN32)
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace liquid::store_detail {
namespace {

error map_error(const std::filesystem::path& path, std::string message) {
    return {errc::io, std::move(message), path.string()};
}

} // namespace

mapped_file::~mapped_file() {
    close();
}

mapped_file::mapped_file(mapped_file&& other) noexcept
    : data_(std::exchange(other.data_, nullptr)), size_(std::exchange(other.size_, 0)),
      owned_(std::move(other.owned_))
#if defined(_WIN32)
      ,
      file_(std::exchange(other.file_, nullptr)), mapping_(std::exchange(other.mapping_, nullptr))
#endif
{
    if (!owned_.empty()) {
        data_ = owned_.data();
    }
}

mapped_file& mapped_file::operator=(mapped_file&& other) noexcept {
    if (this == &other) {
        return *this;
    }
    close();
    data_ = std::exchange(other.data_, nullptr);
    size_ = std::exchange(other.size_, 0);
    owned_ = std::move(other.owned_);
    if (!owned_.empty()) {
        data_ = owned_.data();
    }
#if defined(_WIN32)
    file_ = std::exchange(other.file_, nullptr);
    mapping_ = std::exchange(other.mapping_, nullptr);
#endif
    return *this;
}

result<void> mapped_file::open(const std::filesystem::path& path) {
    close();
#if defined(_WIN32)
    const auto file = CreateFileW(path.c_str(),
                                  GENERIC_READ,
                                  FILE_SHARE_READ,
                                  nullptr,
                                  OPEN_EXISTING,
                                  FILE_ATTRIBUTE_NORMAL,
                                  nullptr);
    if (file == INVALID_HANDLE_VALUE) {
        return std::unexpected(map_error(path, std::system_category().message(GetLastError())));
    }
    LARGE_INTEGER length{};
    if (!GetFileSizeEx(file, &length) || length.QuadPart <= 0) {
        CloseHandle(file);
        return std::unexpected(map_error(path, "cannot map an empty file"));
    }
    const auto mapping = CreateFileMappingW(file, nullptr, PAGE_READONLY, 0, 0, nullptr);
    if (mapping == nullptr) {
        const auto message = std::system_category().message(GetLastError());
        CloseHandle(file);
        return std::unexpected(map_error(path, message));
    }
    const auto view = MapViewOfFile(mapping, FILE_MAP_READ, 0, 0, 0);
    if (view == nullptr) {
        const auto message = std::system_category().message(GetLastError());
        CloseHandle(mapping);
        CloseHandle(file);
        return std::unexpected(map_error(path, message));
    }
    data_ = static_cast<const std::byte*>(view);
    size_ = static_cast<std::size_t>(length.QuadPart);
    file_ = file;
    mapping_ = mapping;
#else
    const auto descriptor = ::open(path.c_str(), O_RDONLY);
    if (descriptor < 0) {
        return std::unexpected(map_error(path, std::system_category().message(errno)));
    }
    struct stat status {};
    if (fstat(descriptor, &status) != 0 || status.st_size <= 0) {
        const auto message = status.st_size <= 0 ? "cannot map an empty file"
                                                 : std::system_category().message(errno);
        ::close(descriptor);
        return std::unexpected(map_error(path, message));
    }
    const auto view = mmap(
        nullptr, static_cast<std::size_t>(status.st_size), PROT_READ, MAP_PRIVATE, descriptor, 0);
    ::close(descriptor);
    if (view == MAP_FAILED) {
        return std::unexpected(map_error(path, std::system_category().message(errno)));
    }
    data_ = static_cast<const std::byte*>(view);
    size_ = static_cast<std::size_t>(status.st_size);
#endif
    return {};
}

runtime::task<void> mapped_file::open(std::shared_ptr<::liquid::store_backend> backend,
                                      std::string key) {
    if (const auto path = backend_local_path(backend, key)) {
        co_return open(*path);
    }
    close();
    auto bytes = co_await read_backend_object(backend, std::move(key));
    if (!bytes) {
        co_return std::unexpected(bytes.error());
    }
    if (bytes->empty()) {
        co_return std::unexpected(error{errc::io, "cannot map an empty backend object", {}});
    }
    owned_ = std::move(*bytes);
    data_ = owned_.data();
    size_ = owned_.size();
    co_return result<void>{};
}

void mapped_file::close() noexcept {
    if (data_ == nullptr) {
        return;
    }
    if (!owned_.empty()) {
        owned_.clear();
        data_ = nullptr;
        size_ = 0;
        return;
    }
#if defined(_WIN32)
    UnmapViewOfFile(data_);
    CloseHandle(mapping_);
    CloseHandle(file_);
    mapping_ = nullptr;
    file_ = nullptr;
#else
    munmap(const_cast<std::byte*>(data_), size_);
#endif
    data_ = nullptr;
    size_ = 0;
}

std::span<const std::byte> mapped_file::bytes() const noexcept {
    return {data_, size_};
}

} // namespace liquid::store_detail
