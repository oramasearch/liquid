// SPDX-License-Identifier: MIT

#include "conformance_backend.hpp"

#if !defined(_WIN32)
#include <algorithm>
#include <atomic>
#include <cerrno>
#include <cstring>
#include <fcntl.h>
#include <map>
#include <set>
#include <unistd.h>

#include "drive/settings.hpp"
#include "vfs/hydration.hpp"
#include "vfs/request_queue.hpp"
#include "vfs/xattrs.hpp"

namespace liquid::test::vfs_conformance {
namespace {

using repository_detail::vfs_access;
using repository_detail::vfs_entry;
using vfs_detail::observed_operation;

auto error_number(const error& failure) -> int {
    switch (failure.code) {
    case errc::not_found:
        return -ENOENT;
    case errc::already_exists:
        return -EEXIST;
    case errc::invalid_argument:
        return -EINVAL;
    case errc::locked:
    case errc::unauthorized:
        return -EACCES;
    case errc::unsupported:
        return -ENOTSUP;
    case errc::unavailable:
        return -EAGAIN;
    default:
        return -EIO;
    }
}

auto missing_attribute() -> int {
#if defined(__APPLE__)
    return -ENOATTR;
#else
    return -ENODATA;
#endif
}

// Test backend for calibration only. This is never a production projection and
// never exempts a real kernel backend from running these same cases. It measures
// agreement with liquid's core, not agreement with a known-good filesystem.
class direct_backend final : public vfs_detail::backend, public filesystem_operations {
public:
    direct_backend(mount_request request, std::filesystem::path repository_root)
        : source_(request.source), mountpoint_(std::move(request.mountpoint)),
          snapshot_(request.input_tree), sink_(std::move(request.observe)),
          read_only_(request.read_only), cache_(repository_root / "LIQUID_VFS" / "chunks"),
          queue_(std::make_shared<vfs_detail::request_queue>()) {
        for (const auto& output : request.outputs) {
            times_[output] = 2'000'000'000;
            if (!read_only_) {
                current_.insert(output);
            }
        }
    }

    auto initialise() -> runtime::task<void> {
        co_return co_await cache_.initialise();
    }

    void request_unmount() noexcept override {
        stopped_ = true;
    }

    auto unmount() -> runtime::task<void> override {
        request_unmount();
        co_return result<void>{};
    }

    auto status() const -> result<mount_status> override {
        auto output = cache_.status();
        output.writes_captured = writes_.load();
        output.request_queue_latency = queue_->average_latency();
        return output;
    }

    auto getattr(const std::filesystem::path& path, struct stat* output) -> int override {
        return submit([this, name = relative(path), output] { return attributes(name, output); });
    }

    auto readdir(const std::filesystem::path& path) -> directory_result override {
        auto output = directory_result{};
        output.opened = submit([this, name = relative(path), &output] {
                            return directory(name, output.entries);
                        }) == 0;
        return output;
    }

    auto open(const std::filesystem::path& path, int flags, mode_t mode) -> int override {
        return submit(
            [this, name = relative(path), flags, mode] { return open_file(name, flags, mode); });
    }

    auto pread(int file, void* into, std::size_t size, off_t offset) -> ssize_t override {
        return submit(
            [this, file, into, size, offset] { return read_file(file, into, size, offset); });
    }

    auto pwrite(int file, const void* from, std::size_t size, off_t offset) -> ssize_t override {
        return submit(
            [this, file, from, size, offset] { return write_file(file, from, size, offset); });
    }

    auto release(int file) -> int override {
        return submit([this, file] { return close_file(file); });
    }

    auto truncate(const std::filesystem::path& path, off_t size) -> int override {
        return submit([this, name = relative(path), size] { return resize_file(name, size); });
    }

    auto chmod(const std::filesystem::path& path, mode_t mode) -> int override {
        return submit([this, name = relative(path), mode] { return permissions(name, mode); });
    }

    auto rename(const std::filesystem::path& from, const std::filesystem::path& to)
        -> int override {
        return submit(
            [this, from = relative(from), to = relative(to)] { return move_file(from, to); });
    }

    auto unlink(const std::filesystem::path& path) -> int override {
        return submit([this, name = relative(path)] { return remove_file(name); });
    }

    auto
    get_attribute(const std::filesystem::path& path, const char* name, char* into, std::size_t size)
        -> ssize_t override {
        return submit([this, path = relative(path), name = std::string{name}, into, size] {
            return read_attribute(path, name, into, size);
        });
    }

    auto list_attributes(const std::filesystem::path& path, char* into, std::size_t size)
        -> ssize_t override {
        return submit([this, path = relative(path), into, size] {
            return read_attribute(path, {}, into, size);
        });
    }

    auto set_attribute(const std::filesystem::path& path, const char* name, std::string_view value)
        -> int override {
        return submit(
            [this, path = relative(path), name = std::string{name}, value = std::string{value}] {
                return change_attribute(path, name, value);
            });
    }

    auto remove_attribute(const std::filesystem::path& path, const char* name) -> int override {
        return submit([this, path = relative(path), name = std::string{name}] {
            return change_attribute(path, name, std::nullopt);
        });
    }

private:
    struct file_handle {
        std::string path;
        int flags;
    };

    auto submit(vfs_detail::request_queue::work work) -> int {
        if (stopped_) {
            errno = EBADF;
            return -1;
        }
        const auto result = queue_->submit(std::move(work));
        if (result < 0) {
            errno = -result;
            return -1;
        }
        return result;
    }

    auto relative(const std::filesystem::path& path) const -> std::string {
        const auto value = path.lexically_relative(mountpoint_).generic_string();
        return value == "." ? std::string{} : value;
    }

    auto entry(const std::string& path) -> runtime::task<vfs_entry> {
        if (deleted_.contains(path)) {
            co_return std::unexpected(error{errc::not_found, "removed path", path});
        }
        if (snapshot_ && !current_.contains(path)) {
            co_return co_await vfs_access::entry(source_, *snapshot_, path);
        }
        co_return co_await vfs_access::entry(source_, path);
    }

    void observe(observed_operation operation,
                 const std::string& path,
                 std::optional<digest> content = {},
                 bool denied = false) {
        if (sink_) {
            sink_({operation, path, ::getpid(), content, denied});
        }
    }

    auto refuse(const std::string& path) -> bool {
        if (!read_only_ && path != "deps" && !path.starts_with("deps/")) {
            return false;
        }
        observe(observed_operation::write, path, {}, true);
        return true;
    }

    void captured(const std::string& path, std::optional<node_id> node = {}) {
        current_.insert(path);
        deleted_.erase(path);
        times_[path] = next_time_++;
        ++writes_;
        if (node) {
            cache_.invalidate(*node);
        }
        observe(observed_operation::write, path);
    }

    auto attributes(std::string path, struct stat* output) -> runtime::task<int> {
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        *output = {};
        output->st_mode = file->mode;
        output->st_size = static_cast<off_t>(file->size);
        output->st_nlink = file->kind == node_kind::blob ? 1 : 2;
        output->st_uid = ::getuid();
        output->st_gid = ::getgid();
        const auto modified = times_.contains(path) ? times_.at(path) : 1'000'000'000;
#if defined(__APPLE__)
        output->st_mtimespec.tv_sec = modified;
#else
        output->st_mtim.tv_sec = modified;
#endif
        observe(observed_operation::getattr, path, file->object);
        co_return 0;
    }

    auto directory(std::string path, std::vector<std::string>& output) -> runtime::task<int> {
        auto values = snapshot_ ? co_await vfs_access::children(source_, *snapshot_, path)
                                : co_await vfs_access::children(source_, path);
        if (!values) {
            co_return error_number(values.error());
        }
        const auto child_path = [&path](const auto& value) {
            return path.empty() ? value.name : path + "/" + value.name;
        };
        std::erase_if(*values, [&](const auto& value) {
            return deleted_.contains(child_path(value)) || current_.contains(child_path(value));
        });
        if (snapshot_) {
            auto live = co_await vfs_access::children(source_, path);
            if (!live) {
                co_return error_number(live.error());
            }
            for (auto& value : *live) {
                if (current_.contains(child_path(value))) {
                    values->push_back(std::move(value));
                }
            }
        } else {
            // Without a snapshot every live entry belongs to the current view.
            values = co_await vfs_access::children(source_, path);
            if (!values) {
                co_return error_number(values.error());
            }
        }
        output = {".", ".."};
        for (const auto& value : *values) {
            output.push_back(value.name);
        }
        observe(observed_operation::readdir, path);
        co_return 0;
    }

    auto open_file(std::string path, int flags, mode_t mode) -> runtime::task<int> {
        const auto writing = (flags & O_ACCMODE) != O_RDONLY;
        if ((writing || (flags & O_CREAT)) && refuse(path)) {
            co_return -EROFS;
        }
        auto file = co_await entry(path);
        if (file && (flags & O_CREAT) && (flags & O_EXCL)) {
            co_return -EEXIST;
        }
        if (!file && file.error().code == errc::not_found && (flags & O_CREAT)) {
            auto written = co_await source_.write(path, {}, {.mode = S_IFREG | (mode & 0777U)});
            if (!written) {
                co_return error_number(written.error());
            }
            captured(path);
            file = co_await entry(path);
        }
        if (!file) {
            co_return error_number(file.error());
        }
        if (file->kind != node_kind::blob) {
            co_return -EISDIR;
        }
        if (writing && (flags & O_TRUNC)) {
            const auto resized = co_await resize_file(path, 0);
            if (!resized) {
                co_return error_number(resized.error());
            }
            if (*resized < 0) {
                co_return *resized;
            }
        }
        const auto id = next_handle_++;
        handles_.emplace(id, file_handle{path, flags});
        observe(observed_operation::open, path, file->object);
        co_return id;
    }

    auto close_file(int file) -> runtime::task<int> {
        co_return handles_.erase(file) == 1 ? 0 : -EBADF;
    }

    auto read_file(int id, void* into, std::size_t size, off_t offset) -> runtime::task<int> {
        const auto handle = handles_.find(id);
        if (handle == handles_.end() || (handle->second.flags & O_ACCMODE) == O_WRONLY) {
            co_return -EBADF;
        }
        if (offset < 0) {
            co_return -EINVAL;
        }
        const auto path = handle->second.path;
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        auto content = co_await cache_.read(source_, *file, offset, size);
        if (!content) {
            co_return error_number(content.error());
        }
        if (!content->empty()) {
            std::memcpy(into, content->data(), content->size());
        }
        observe(observed_operation::read, path, file->object);
        co_return static_cast<int>(content->size());
    }

    auto write_file(int id, const void* from, std::size_t size, off_t offset)
        -> runtime::task<int> {
        const auto handle = handles_.find(id);
        if (handle == handles_.end() || (handle->second.flags & O_ACCMODE) == O_RDONLY) {
            co_return -EBADF;
        }
        const auto path = handle->second.path;
        if (refuse(path)) {
            co_return -EROFS;
        }
        if (offset < 0) {
            co_return -EINVAL;
        }
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        auto content = co_await cache_.read(source_, *file, 0, file->size);
        if (!content) {
            co_return error_number(content.error());
        }
        content->resize(std::max(content->size(), static_cast<std::size_t>(offset) + size));
        if (size != 0) {
            std::memcpy(content->data() + offset, from, size);
        }
        auto written = co_await source_.write(path, std::move(*content), {.mode = file->mode});
        if (!written) {
            co_return error_number(written.error());
        }
        captured(path, file->node);
        co_return static_cast<int>(size);
    }

    auto resize_file(std::string path, off_t size) -> runtime::task<int> {
        if (refuse(path)) {
            co_return -EROFS;
        }
        if (size < 0) {
            co_return -EINVAL;
        }
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        auto content = co_await cache_.read(source_, *file, 0, file->size);
        if (!content) {
            co_return error_number(content.error());
        }
        content->resize(size);
        auto written = co_await source_.write(path, std::move(*content), {.mode = file->mode});
        if (!written) {
            co_return error_number(written.error());
        }
        captured(path, file->node);
        co_return 0;
    }

    auto permissions(std::string path, mode_t mode) -> runtime::task<int> {
        if (refuse(path)) {
            co_return -EROFS;
        }
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        auto changed =
            co_await vfs_access::set_mode(source_, path, (file->mode & S_IFMT) | (mode & 0777));
        if (!changed) {
            co_return error_number(changed.error());
        }
        captured(path);
        co_return 0;
    }

    auto move_file(std::string from, std::string to) -> runtime::task<int> {
        if (refuse(from) || refuse(to)) {
            co_return -EROFS;
        }
        auto file = co_await entry(from);
        if (!file) {
            co_return error_number(file.error());
        }
        auto destination = co_await entry(to);
        if (destination) {
            auto removed = co_await source_.remove(to);
            if (!removed) {
                co_return error_number(removed.error());
            }
            cache_.invalidate(destination->node);
        } else if (destination.error().code != errc::not_found) {
            co_return error_number(destination.error());
        }
        auto moved = co_await source_.move(from, to);
        if (!moved) {
            co_return error_number(moved.error());
        }
        current_.erase(from);
        deleted_.insert(from);
        captured(to);
        times_[from] = next_time_++;
        observe(observed_operation::write, from);
        co_return 0;
    }

    auto remove_file(std::string path) -> runtime::task<int> {
        if (refuse(path)) {
            co_return -EROFS;
        }
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        auto removed = co_await source_.remove(path);
        if (!removed) {
            co_return error_number(removed.error());
        }
        captured(path, file->node);
        current_.erase(path);
        deleted_.insert(path);
        co_return 0;
    }

    auto read_attribute(std::string path, std::string name, char* into, std::size_t size)
        -> runtime::task<int> {
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        auto settings = co_await drive_detail::load_settings(source_);
        if (!settings) {
            co_return error_number(settings.error());
        }
        auto value = name.empty() ? result<std::string>{vfs_detail::drive_xattr_names()}
                                  : vfs_detail::drive_xattr_value(
                                        drive_detail::effective_state(*settings, path), name);
        if (!value) {
            co_return missing_attribute();
        }
        if (size != 0 && size < value->size()) {
            co_return -ERANGE;
        }
        if (size != 0) {
            std::memcpy(into, value->data(), value->size());
        }
        co_return static_cast<int>(value->size());
    }

    auto change_attribute(std::string path, std::string name, std::optional<std::string> value)
        -> runtime::task<int> {
        auto file = co_await entry(path);
        if (!file) {
            co_return error_number(file.error());
        }
        if (name != vfs_detail::pin_xattr && name != vfs_detail::state_xattr) {
            co_return missing_attribute();
        }
        auto state = value ? vfs_detail::drive_xattr_state(name, *value)
                           : result<drive_path_state>{drive_path_state::online_only};
        if (!state) {
            co_return error_number(state.error());
        }
        auto configured = co_await drive_detail::set_path_state(source_, path, *state);
        if (!configured) {
            co_return error_number(configured.error());
        }
        if (*state == drive_path_state::available_offline) {
            auto hydrated = co_await cache_.hydrate(source_, *file);
            if (!hydrated) {
                co_return error_number(hydrated.error());
            }
            auto recorded = co_await drive_detail::record_hydrated(source_, path);
            if (!recorded) {
                co_return error_number(recorded.error());
            }
        }
        co_return 0;
    }

    repository& source_;
    std::filesystem::path mountpoint_;
    std::optional<digest> snapshot_;
    vfs_detail::observation_sink sink_;
    bool read_only_;
    vfs_detail::hydration_cache cache_;
    std::shared_ptr<vfs_detail::request_queue> queue_;
    std::atomic_bool stopped_{};
    std::atomic_uint64_t writes_{};
    std::set<std::string> current_;
    std::set<std::string> deleted_;
    std::map<std::string, std::int64_t> times_;
    std::int64_t next_time_{2'000'000'001};
    std::map<int, file_handle> handles_;
    int next_handle_{1};
};

} // namespace

auto start_direct(mount_request request) -> runtime::task<backend_instance> {
    auto root = vfs_access::root_path(request.source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto backend = std::make_shared<direct_backend>(std::move(request), *root);
    auto initialised = co_await backend->initialise();
    if (!initialised) {
        co_return std::unexpected(initialised.error());
    }
    co_return backend_instance{backend, backend};
}

} // namespace liquid::test::vfs_conformance
#else
namespace liquid::test::vfs_conformance {
auto start_direct(mount_request) -> runtime::task<backend_instance> {
    co_return std::unexpected(
        error{errc::unsupported, "conformance requires POSIX operation types"});
}
} // namespace liquid::test::vfs_conformance
#endif
