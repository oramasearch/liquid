#include "backend.hpp"

#if defined(LIQUID_HAS_FUSE3)
#define FUSE_USE_VERSION 35
#include <fuse.h>
#include <fuse_lowlevel.h>
#elif defined(LIQUID_HAS_FUSE2)
#define FUSE_USE_VERSION 29
#include <fuse.h>
#endif

#if defined(LIQUID_HAS_FUSE3) || defined(LIQUID_HAS_FUSE2)

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <condition_variable>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <iostream>
#include <mutex>
#include <optional>
#include <string>
#include <sys/stat.h>
#include <sys/statvfs.h>
#include <unistd.h>
#include <vector>

#include "hydration.hpp"
#include "request_queue.hpp"
#include "vfs/repository_access.hpp"

namespace liquid::vfs_detail {
namespace {

auto fuse_path(const char* path) -> std::string {
    auto output = std::string{path ? path : ""};
    while (output.starts_with('/'))
        output.erase(output.begin());
    return output;
}

auto fuse_error(const error& value, bool reading = false) -> int {
    if (reading) {
        std::cerr << "liquid VFS: " << value.message << ": " << value.context << '\n';
        return -EIO;
    }
    auto number = EIO;
    switch (value.code) {
    case errc::not_found:
        number = ENOENT;
        break;
    case errc::already_exists:
        number = EEXIST;
        break;
    case errc::invalid_argument:
        number = EINVAL;
        break;
    case errc::locked:
    case errc::unauthorized:
        number = EACCES;
        break;
    case errc::unsupported:
        number = ENOTSUP;
        break;
    case errc::busy:
        number = EBUSY;
        break;
    default:
        number = EIO;
        break;
    }
    if (number == EIO)
        std::cerr << "liquid VFS: " << value.message << ": " << value.context << '\n';
    return -number;
}

auto prepare_mountpoint(const std::filesystem::path& path) -> result<void> {
    auto code = std::error_code{};
    if (!std::filesystem::exists(path, code))
        std::filesystem::create_directories(path, code);
    if (code)
        return std::unexpected(error{errc::io, code.message(), path.string()});
    if (!std::filesystem::is_directory(path, code))
        return std::unexpected(
            error{errc::invalid_argument, "mountpoint is not a directory", path.string()});
    if (!std::filesystem::is_empty(path, code))
        return std::unexpected(
            error{errc::already_exists, "mountpoint is not empty", path.string()});
    if (code)
        return std::unexpected(error{errc::io, code.message(), path.string()});
    return {};
}

runtime::task<void> wake_fuse_loop(std::filesystem::path mountpoint) {
    auto awakened = co_await runtime::offload([mountpoint = std::move(mountpoint)] {
        struct stat attributes{};
        static_cast<void>(::lstat(mountpoint.c_str(), &attributes));
    });
    if (!awakened)
        co_return std::unexpected(awakened.error());
    co_return {};
}

class fuse_backend final : public backend {
public:
    fuse_backend(repository& source,
                 std::filesystem::path mountpoint,
                 std::filesystem::path repository_root,
                 mount_options options)
        : source_(source), mountpoint_(std::move(mountpoint)),
          repository_root_(std::move(repository_root)), options_(options),
          cache_(repository_root_ / "LIQUID_VFS" / "chunks"),
          queue_(std::make_shared<request_queue>()) {}

    void request_unmount() noexcept override {
        auto wake_loop = false;
        {
            auto lock = std::scoped_lock{lifecycle_mutex_};
            stop_requested_ = true;
            if (fuse_ && !wake_scheduled_) {
                fuse_exit(fuse_);
                wake_scheduled_ = true;
                wake_loop = true;
            }
        }
        if (wake_loop)
            runtime::spawn(wake_fuse_loop(mountpoint_), [](result<void>) {});
    }

    runtime::task<void> unmount() override {
        request_unmount();
        auto waited = co_await runtime::offload([this] { return wait_stopped(); });
        if (!waited)
            co_return std::unexpected(waited.error());
        co_return std::move(*waited);
    }

    auto status() const -> result<mount_status> override {
        auto output = cache_.status();
        output.writes_captured = writes_captured_.load();
        output.request_queue_latency = queue_->average_latency();
        return output;
    }

    auto initialise_cache() -> runtime::task<void> {
        co_return co_await cache_.initialise();
    }

    auto run() -> result<void> {
        struct fuse_args arguments = FUSE_ARGS_INIT(0, nullptr);
        const auto timeout = "attr_timeout=" + std::to_string(options_.attr_timeout.count()) +
                             ",entry_timeout=" + std::to_string(options_.attr_timeout.count());
        auto arguments_valid = fuse_opt_add_arg(&arguments, "liquid") == 0;
#if defined(LIQUID_HAS_FUSE3)
        arguments_valid = arguments_valid && fuse_opt_add_arg(&arguments, "-f") == 0;
        arguments_valid = arguments_valid && fuse_opt_add_arg(&arguments, mountpoint_.c_str()) == 0;
#endif
        arguments_valid = arguments_valid && fuse_opt_add_arg(&arguments, "-o") == 0;
        arguments_valid = arguments_valid && fuse_opt_add_arg(&arguments, timeout.c_str()) == 0;
        if (!arguments_valid) {
            fuse_opt_free_args(&arguments);
            auto failure = error{errc::io, "cannot build FUSE arguments", mountpoint_.string()};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
#if defined(LIQUID_HAS_FUSE3)
        struct fuse_cmdline_opts command{};
        if (fuse_parse_cmdline(&arguments, &command) != 0 || !command.foreground) {
            std::free(command.mountpoint);
            fuse_opt_free_args(&arguments);
            auto failure = error{errc::io, "cannot parse FUSE arguments", mountpoint_.string()};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
#endif
        auto operations = make_operations();
#if defined(LIQUID_HAS_FUSE3)
        auto* instance = fuse_new(&arguments, &operations, sizeof(operations), this);
        fuse_opt_free_args(&arguments);
        if (!instance) {
            std::free(command.mountpoint);
            auto failure = error{errc::unsupported,
                                 "cannot create FUSE session",
                                 "verify that FUSE is installed and available"};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
#else
        auto* channel = fuse_mount(mountpoint_.c_str(), &arguments);
        if (!channel) {
            fuse_opt_free_args(&arguments);
            auto failure = error{errc::unsupported,
                                 "cannot mount macFUSE filesystem",
                                 "verify that macFUSE is installed and available"};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
        auto* instance = fuse_new(channel, &arguments, &operations, sizeof(operations), this);
        fuse_opt_free_args(&arguments);
        if (!instance) {
            fuse_unmount(mountpoint_.c_str(), channel);
            auto failure = error{errc::unsupported,
                                 "cannot create macFUSE session",
                                 "verify that macFUSE is installed and available"};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
#endif
#if defined(LIQUID_HAS_FUSE3)
        if (fuse_mount(instance, command.mountpoint) != 0) {
            std::free(command.mountpoint);
            fuse_destroy(instance);
            auto failure = error{errc::unsupported,
                                 "cannot mount FUSE filesystem",
                                 "verify /dev/fuse access and the FUSE installation"};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
        std::free(command.mountpoint);
        auto* loop_config = fuse_loop_cfg_create();
        if (!loop_config) {
            fuse_unmount(instance);
            fuse_destroy(instance);
            auto failure =
                error{errc::io, "cannot configure FUSE request loop", mountpoint_.string()};
            set_ready(failure);
            set_stopped(failure);
            return std::unexpected(std::move(failure));
        }
        fuse_loop_cfg_set_clone_fd(loop_config, command.clone_fd);
#endif
        auto stop_now = false;
        {
            auto lock = std::scoped_lock{lifecycle_mutex_};
            fuse_ = instance;
            ready_ = true;
            stop_now = stop_requested_;
        }
        lifecycle_changed_.notify_all();
        if (stop_now)
            request_unmount();
#if defined(LIQUID_HAS_FUSE3)
        const auto loop_result = fuse_loop_mt(instance, loop_config);
        fuse_loop_cfg_destroy(loop_config);
#else
        const auto loop_result = fuse_loop_mt(instance);
#endif
#if defined(LIQUID_HAS_FUSE3)
        fuse_unmount(instance);
#else
        fuse_unmount(mountpoint_.c_str(), channel);
#endif
        fuse_destroy(instance);
        auto outcome = result<void>{};
        if (loop_result != 0)
            outcome =
                std::unexpected(error{errc::io, "FUSE request loop failed", mountpoint_.string()});
        set_stopped(outcome ? std::optional<error>{} : std::optional<error>{outcome.error()});
        return outcome;
    }

    auto wait_ready() -> result<void> {
        auto lock = std::unique_lock{lifecycle_mutex_};
        lifecycle_changed_.wait(lock, [this] { return ready_ || startup_error_.has_value(); });
        if (startup_error_)
            return std::unexpected(*startup_error_);
        return {};
    }

    auto queue() const -> const std::shared_ptr<request_queue>& {
        return queue_;
    }

    auto source() -> repository& {
        return source_;
    }

    auto cache() -> hydration_cache& {
        return cache_;
    }

    auto read_only() const -> bool {
        return options_.read_only;
    }

    void captured() {
        ++writes_captured_;
    }

private:
    static auto make_operations() -> fuse_operations;

    void set_ready(error failure) {
        {
            auto lock = std::scoped_lock{lifecycle_mutex_};
            startup_error_ = std::move(failure);
        }
        lifecycle_changed_.notify_all();
    }

    void set_stopped(std::optional<error> failure) {
        {
            auto lock = std::scoped_lock{lifecycle_mutex_};
            fuse_ = nullptr;
            stopped_ = true;
            if (failure)
                run_error_ = std::move(*failure);
        }
        lifecycle_changed_.notify_all();
    }

    auto wait_stopped() -> result<void> {
        auto lock = std::unique_lock{lifecycle_mutex_};
        lifecycle_changed_.wait(lock, [this] { return stopped_; });
        if (run_error_)
            return std::unexpected(*run_error_);
        return {};
    }

    repository& source_;
    std::filesystem::path mountpoint_;
    std::filesystem::path repository_root_;
    mount_options options_;
    hydration_cache cache_;
    std::shared_ptr<request_queue> queue_;
    std::atomic_uint64_t writes_captured_{};
    mutable std::mutex lifecycle_mutex_;
    std::condition_variable lifecycle_changed_;
    struct fuse* fuse_{};
    bool ready_{};
    bool stopped_{};
    bool stop_requested_{};
    bool wake_scheduled_{};
    std::optional<error> startup_error_;
    std::optional<error> run_error_;
};

auto current_backend() -> fuse_backend* {
    const auto* context = fuse_get_context();
    return context ? static_cast<fuse_backend*>(context->private_data) : nullptr;
}

runtime::task<int>
getattr_request(fuse_backend* state, std::string path, std::shared_ptr<struct stat> output) {
    auto entry = co_await repository_detail::vfs_access::entry(state->source(), std::move(path));
    if (!entry)
        co_return fuse_error(entry.error());
    std::memset(output.get(), 0, sizeof(struct stat));
    output->st_mode = static_cast<mode_t>(entry->mode);
    output->st_nlink = entry->kind == node_kind::blob ? 1 : 2;
    output->st_size = static_cast<off_t>(entry->size);
    output->st_uid = getuid();
    output->st_gid = getgid();
    co_return 0;
}

runtime::task<int> readdir_request(fuse_backend* state,
                                   std::string path,
                                   std::shared_ptr<std::vector<std::string>> output) {
    auto entries =
        co_await repository_detail::vfs_access::children(state->source(), std::move(path));
    if (!entries)
        co_return fuse_error(entries.error());
    output->reserve(entries->size() + 2);
    output->push_back(".");
    output->push_back("..");
    for (const auto& entry : *entries)
        output->push_back(entry.name);
    co_return 0;
}

runtime::task<int> open_request(fuse_backend* state, std::string path, int flags) {
    auto entry = co_await repository_detail::vfs_access::entry(state->source(), std::move(path));
    if (!entry)
        co_return fuse_error(entry.error());
    if (entry->kind != node_kind::blob)
        co_return -EISDIR;
    if (state->read_only() && (flags & (O_WRONLY | O_RDWR)) != 0)
        co_return -EROFS;
    co_return 0;
}

runtime::task<int> read_request(fuse_backend* state,
                                std::string path,
                                std::uint64_t offset,
                                std::size_t size,
                                std::shared_ptr<std::vector<std::byte>> output) {
    auto entry = co_await repository_detail::vfs_access::entry(state->source(), std::move(path));
    if (!entry)
        co_return fuse_error(entry.error());
    auto bytes = co_await state->cache().read(state->source(), *entry, offset, size);
    if (!bytes)
        co_return fuse_error(bytes.error(), true);
    *output = std::move(*bytes);
    co_return static_cast<int>(output->size());
}

runtime::task<int> create_request(fuse_backend* state, std::string path, std::uint32_t mode) {
    if (state->read_only())
        co_return -EROFS;
    auto written = co_await state->source().write(path, {});
    if (!written)
        co_return fuse_error(written.error());
    auto mode_set = co_await repository_detail::vfs_access::set_mode(
        state->source(), path, static_cast<std::uint32_t>(S_IFREG) | mode);
    if (!mode_set)
        co_return fuse_error(mode_set.error());
    state->captured();
    co_return 0;
}

runtime::task<int> write_request(fuse_backend* state,
                                 std::string path,
                                 std::uint64_t offset,
                                 std::vector<std::byte> input) {
    if (state->read_only())
        co_return -EROFS;
    auto entry = co_await repository_detail::vfs_access::entry(state->source(), path);
    if (!entry)
        co_return fuse_error(entry.error());
    if (entry->kind != node_kind::blob)
        co_return -EISDIR;
    auto contents = co_await state->cache().read(state->source(), *entry, 0, entry->size);
    if (!contents)
        co_return fuse_error(contents.error(), true);
    const auto end = offset + input.size();
    if (end > contents->size())
        contents->resize(static_cast<std::size_t>(end));
    std::copy(input.begin(), input.end(), contents->begin() + static_cast<std::size_t>(offset));
    auto written = co_await state->source().write(path, std::move(*contents));
    if (!written)
        co_return fuse_error(written.error());
    state->cache().invalidate(entry->node);
    state->captured();
    co_return static_cast<int>(input.size());
}

runtime::task<int> remove_request(fuse_backend* state, std::string path, bool directory) {
    if (state->read_only())
        co_return -EROFS;
    auto entry = co_await repository_detail::vfs_access::entry(state->source(), path);
    if (!entry)
        co_return fuse_error(entry.error());
    if (directory && entry->kind == node_kind::blob)
        co_return -ENOTDIR;
    if (!directory && entry->kind != node_kind::blob)
        co_return -EISDIR;
    if (directory) {
        auto children = co_await repository_detail::vfs_access::children(state->source(), path);
        if (!children)
            co_return fuse_error(children.error());
        if (!children->empty())
            co_return -ENOTEMPTY;
    }
    auto removed = co_await state->source().remove(path);
    if (!removed)
        co_return fuse_error(removed.error());
    state->cache().invalidate(entry->node);
    state->captured();
    co_return 0;
}

runtime::task<int> rename_request(fuse_backend* state, std::string from, std::string to) {
    if (state->read_only())
        co_return -EROFS;
    auto moved = co_await state->source().move(std::move(from), std::move(to));
    if (!moved)
        co_return fuse_error(moved.error());
    state->captured();
    co_return 0;
}

runtime::task<int> mkdir_request(fuse_backend* state, std::string path, std::uint32_t mode) {
    if (state->read_only())
        co_return -EROFS;
    auto created = co_await repository_detail::vfs_access::make_directory(
        state->source(), std::move(path), static_cast<std::uint32_t>(S_IFDIR) | mode);
    if (!created)
        co_return fuse_error(created.error());
    state->captured();
    co_return 0;
}

runtime::task<int> statfs_request(std::shared_ptr<struct statvfs> output) {
    std::memset(output.get(), 0, sizeof(struct statvfs));
    output->f_bsize = 4096;
    output->f_frsize = 4096;
    output->f_blocks = 1024ULL * 1024ULL * 1024ULL;
    output->f_bfree = output->f_blocks;
    output->f_bavail = output->f_blocks;
    output->f_files = 1024ULL * 1024ULL * 1024ULL;
    output->f_ffree = output->f_files;
    output->f_namemax = 255;
    co_return 0;
}

#if defined(LIQUID_HAS_FUSE3)
int fuse_getattr(const char* path, struct stat* output, struct fuse_file_info*) {
#else
int fuse_getattr(const char* path, struct stat* output) {
#endif
    auto* state = current_backend();
    if (!state)
        return -EIO;
    auto value = std::make_shared<struct stat>();
    const auto result = state->queue()->submit(
        [state, path = fuse_path(path), value] { return getattr_request(state, path, value); });
    if (result == 0)
        *output = *value;
    return result;
}

#if defined(LIQUID_HAS_FUSE3)
int fuse_readdir(const char* path,
                 void* buffer,
                 fuse_fill_dir_t filler,
                 off_t,
                 struct fuse_file_info*,
                 enum fuse_readdir_flags) {
#else
int fuse_readdir(
    const char* path, void* buffer, fuse_fill_dir_t filler, off_t, struct fuse_file_info*) {
#endif
    auto* state = current_backend();
    if (!state)
        return -EIO;
    auto entries = std::make_shared<std::vector<std::string>>();
    const auto result = state->queue()->submit(
        [state, path = fuse_path(path), entries] { return readdir_request(state, path, entries); });
    if (result != 0)
        return result;
    for (const auto& entry : *entries) {
#if defined(LIQUID_HAS_FUSE3)
        if (filler(buffer, entry.c_str(), nullptr, 0, static_cast<enum fuse_fill_dir_flags>(0)) !=
            0)
#else
        if (filler(buffer, entry.c_str(), nullptr, 0) != 0)
#endif
            break;
    }
    return 0;
}

int fuse_open(const char* path, struct fuse_file_info* information) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), flags = information->flags] {
        return open_request(state, path, flags);
    });
}

int fuse_read(const char* path, char* buffer, size_t size, off_t offset, struct fuse_file_info*) {
    auto* state = current_backend();
    if (!state || offset < 0)
        return -EIO;
    auto output = std::make_shared<std::vector<std::byte>>();
    const auto result =
        state->queue()->submit([state, path = fuse_path(path), offset, size, output] {
            return read_request(state, path, static_cast<std::uint64_t>(offset), size, output);
        });
    if (result > 0)
        std::memcpy(buffer, output->data(), static_cast<std::size_t>(result));
    return result;
}

int fuse_create(const char* path, mode_t mode, struct fuse_file_info*) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), mode] {
        return create_request(state, path, static_cast<std::uint32_t>(mode));
    });
}

int fuse_write(
    const char* path, const char* buffer, size_t size, off_t offset, struct fuse_file_info*) {
    auto* state = current_backend();
    if (!state || offset < 0)
        return -EIO;
    auto input = std::vector<std::byte>(reinterpret_cast<const std::byte*>(buffer),
                                        reinterpret_cast<const std::byte*>(buffer + size));
    return state->queue()->submit(
        [state, path = fuse_path(path), offset, input = std::move(input)]() mutable {
            return write_request(state, path, static_cast<std::uint64_t>(offset), std::move(input));
        });
}

int fuse_unlink(const char* path) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit(
        [state, path = fuse_path(path)] { return remove_request(state, path, false); });
}

#if defined(LIQUID_HAS_FUSE3)
int fuse_rename(const char* from, const char* to, unsigned int flags) {
    if (flags != 0)
        return -EINVAL;
#else
int fuse_rename(const char* from, const char* to) {
#endif
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, from = fuse_path(from), to = fuse_path(to)] {
        return rename_request(state, from, to);
    });
}

int fuse_mkdir(const char* path, mode_t mode) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), mode] {
        return mkdir_request(state, path, static_cast<std::uint32_t>(mode));
    });
}

int fuse_rmdir(const char* path) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit(
        [state, path = fuse_path(path)] { return remove_request(state, path, true); });
}

int fuse_statfs(const char*, struct statvfs* output) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    auto value = std::make_shared<struct statvfs>();
    const auto result = state->queue()->submit([value] { return statfs_request(value); });
    if (result == 0)
        *output = *value;
    return result;
}

auto fuse_backend::make_operations() -> fuse_operations {
    auto operations = fuse_operations{};
    operations.getattr = fuse_getattr;
    operations.readdir = fuse_readdir;
    operations.open = fuse_open;
    operations.read = fuse_read;
    operations.create = fuse_create;
    operations.write = fuse_write;
    operations.unlink = fuse_unlink;
    operations.rename = fuse_rename;
    operations.mkdir = fuse_mkdir;
    operations.rmdir = fuse_rmdir;
    operations.statfs = fuse_statfs;
    return operations;
}

runtime::task<void> run_fuse(std::shared_ptr<fuse_backend> state) {
    auto outcome = co_await runtime::offload([state] { return state->run(); });
    if (!outcome)
        co_return std::unexpected(outcome.error());
    co_return std::move(*outcome);
}

} // namespace

runtime::task<std::shared_ptr<backend>>
start_fuse(repository& source, std::filesystem::path mountpoint, mount_options options) {
    auto root = repository_detail::vfs_access::root_path(source);
    if (!root)
        co_return std::unexpected(root.error());
    auto prepared =
        co_await runtime::offload([mountpoint] { return prepare_mountpoint(mountpoint); });
    if (!prepared)
        co_return std::unexpected(prepared.error());
    if (!*prepared)
        co_return std::unexpected(prepared->error());
    auto state =
        std::make_shared<fuse_backend>(source, std::move(mountpoint), std::move(*root), options);
    auto initialised = co_await state->initialise_cache();
    if (!initialised)
        co_return std::unexpected(initialised.error());
    runtime::spawn(run_fuse(state), [state](result<void> outcome) {
        if (!outcome)
            state->request_unmount();
    });
    auto ready = co_await runtime::offload([state] { return state->wait_ready(); });
    if (!ready)
        co_return std::unexpected(ready.error());
    if (!*ready)
        co_return std::unexpected(ready->error());
    co_return std::static_pointer_cast<backend>(std::move(state));
}

} // namespace liquid::vfs_detail

#else

namespace liquid::vfs_detail {

runtime::task<std::shared_ptr<backend>>
start_fuse(repository&, std::filesystem::path, mount_options) {
#if defined(_WIN32)
    co_return std::unexpected(
        error{errc::unsupported,
              "virtual mounts are unavailable on Windows in Phase 1; use liquid::vfs::materialise",
              "liquid #6"});
#elif defined(__APPLE__)
    co_return std::unexpected(
        error{errc::unsupported,
              "macFUSE was not found at build time; install macFUSE and rebuild liquid",
              "liquid #6"});
#else
    co_return std::unexpected(error{errc::unsupported,
                                    "libfuse3 was not found at build time; install libfuse3-dev",
                                    "liquid #6"});
#endif
}

} // namespace liquid::vfs_detail

#endif
