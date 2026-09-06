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
#include "vfs/observation.hpp"
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
        const auto wake_path = mountpoint / ".liquid-unmount-wake";
        static_cast<void>(::lstat(wake_path.c_str(), &attributes));
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
                 mount_options options,
                 observation_sink sink = {},
                 std::vector<std::string> initial_outputs = {},
                 std::optional<digest> snapshot_root = {})
        : source_(source), mountpoint_(std::move(mountpoint)),
          repository_root_(std::move(repository_root)), options_(options),
          cache_(repository_root_ / "LIQUID_VFS" / "chunks"),
          queue_(std::make_shared<request_queue>()), sink_(std::move(sink)),
          snapshot_root_(std::move(snapshot_root)) {
        for (auto& output : initial_outputs) {
            const auto directory = output.ends_with('/');
            auto path = std::filesystem::path{directory ? output.substr(0, output.size() - 1)
                                                        : std::move(output)};
            const auto normalized = path.generic_string();
            modification_times_.insert_or_assign(normalized, 2'000'000'000);
            if (!snapshot_root_) {
                continue;
            }
            if (!options_.read_only) {
                current_entries_.insert_or_assign(normalized, directory);
                continue;
            }
            auto parent = path.parent_path();
            while (!parent.empty()) {
                virtual_entries_.insert_or_assign(parent.generic_string(), node_kind::tree);
                parent = parent.parent_path();
            }
            virtual_entries_.insert_or_assign(normalized,
                                              directory ? node_kind::tree : node_kind::blob);
        }
    }

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

    auto entry(std::string path) -> runtime::task<repository_detail::vfs_entry> {
        if (deleted_path(path)) {
            co_return std::unexpected(
                error{errc::not_found, "entry was removed during execution", path});
        }
        auto value = result<repository_detail::vfs_entry>{};
        if (current_path(path)) {
            value = co_await repository_detail::vfs_access::entry(source_, path);
        } else if (snapshot_root_) {
            value = co_await repository_detail::vfs_access::entry(source_, *snapshot_root_, path);
        } else {
            value = co_await repository_detail::vfs_access::entry(source_, path);
        }
        if (value || value.error().code != errc::not_found) {
            co_return value;
        }
        const auto found = virtual_entries_.find(path);
        if (found == virtual_entries_.end()) {
            co_return std::unexpected(value.error());
        }
        const auto mode = found->second == node_kind::tree ? 0040755U : 0100644U;
        co_return repository_detail::vfs_entry{
            .name = std::filesystem::path{path}.filename().generic_string(),
            .kind = found->second,
            .mode = mode};
    }

    auto children(std::string path) -> runtime::task<std::vector<repository_detail::vfs_entry>> {
        auto values = result<std::vector<repository_detail::vfs_entry>>{};
        const auto current_directory = current_path(path);
        if (current_directory) {
            values = co_await repository_detail::vfs_access::children(source_, path);
        } else if (snapshot_root_) {
            values =
                co_await repository_detail::vfs_access::children(source_, *snapshot_root_, path);
        } else {
            values = co_await repository_detail::vfs_access::children(source_, path);
        }
        if (!values) {
            const auto directory = virtual_entries_.find(path);
            if (values.error().code != errc::not_found || directory == virtual_entries_.end() ||
                directory->second != node_kind::tree) {
                co_return std::unexpected(values.error());
            }
            values = std::vector<repository_detail::vfs_entry>{};
        }
        if (!current_directory && current_below(path)) {
            std::erase_if(*values, [this, &path](const auto& entry) {
                const auto candidate = path.empty() ? entry.name : path + "/" + entry.name;
                return current_path(candidate) || current_below(candidate);
            });
            auto current = co_await repository_detail::vfs_access::children(source_, path);
            if (current) {
                for (auto& entry : *current) {
                    const auto candidate = path.empty() ? entry.name : path + "/" + entry.name;
                    if (!current_path(candidate) && !current_below(candidate)) {
                        continue;
                    }
                    std::erase_if(*values,
                                  [&entry](const auto& value) { return value.name == entry.name; });
                    values->push_back(std::move(entry));
                }
            } else if (current.error().code != errc::not_found) {
                co_return std::unexpected(current.error());
            }
        }
        std::erase_if(*values, [this, &path](const auto& entry) {
            const auto candidate = path.empty() ? entry.name : path + "/" + entry.name;
            return deleted_path(candidate);
        });
        for (const auto& [candidate, kind] : virtual_entries_) {
            const auto candidate_path = std::filesystem::path{candidate};
            if (candidate_path.parent_path().generic_string() != path) {
                continue;
            }
            if (deleted_path(candidate)) {
                continue;
            }
            const auto name = candidate_path.filename().generic_string();
            if (std::ranges::any_of(*values,
                                    [&name](const auto& entry) { return entry.name == name; })) {
                continue;
            }
            const auto mode = kind == node_kind::tree ? 0040755U : 0100644U;
            values->push_back({.name = name, .kind = kind, .mode = mode});
        }
        co_return values;
    }

    auto cache() -> hydration_cache& {
        return cache_;
    }

    auto read_only() const -> bool {
        return options_.read_only;
    }

    auto immutable(std::string_view path) const -> bool {
        return path == "deps" || path.starts_with("deps/");
    }

    void mark_current(std::string path, bool recursive = false) {
        auto lock = std::scoped_lock{overlay_mutex_};
        deleted_entries_.erase(path);
        current_entries_.insert_or_assign(std::move(path), recursive);
    }

    void mark_deleted(std::string path, bool recursive = false) {
        auto lock = std::scoped_lock{overlay_mutex_};
        std::erase_if(current_entries_, [&path](const auto& value) {
            return value.first == path || value.first.starts_with(path + "/");
        });
        deleted_entries_.insert_or_assign(std::move(path), recursive);
    }

    void captured() {
        ++writes_captured_;
    }

    auto modification_time(std::string_view path) const -> std::int64_t {
        auto lock = std::scoped_lock{modification_mutex_};
        const auto found = modification_times_.find(path);
        return found == modification_times_.end() ? 1'000'000'000 : found->second;
    }

    void touch(std::string path) {
        auto lock = std::scoped_lock{modification_mutex_};
        modification_times_.insert_or_assign(std::move(path), next_modification_time_++);
    }

    void observe(observed_operation operation,
                 std::string path,
                 std::int64_t pid,
                 std::optional<digest> content = {},
                 bool denied = false) {
        if (!sink_)
            return;
        sink_({operation, std::move(path), pid, std::move(content), denied});
    }

private:
    static auto make_operations() -> fuse_operations;

    static auto path_matches(std::string_view path, std::string_view root, bool recursive) -> bool {
        return path == root || (recursive && path.starts_with(std::string{root} + "/"));
    }

    auto current_path(std::string_view path) const -> bool {
        auto lock = std::scoped_lock{overlay_mutex_};
        return std::ranges::any_of(current_entries_, [&path](const auto& value) {
            return path_matches(path, value.first, value.second);
        });
    }

    auto current_below(std::string_view path) const -> bool {
        const auto prefix = path.empty() ? std::string{} : std::string{path} + "/";
        auto lock = std::scoped_lock{overlay_mutex_};
        return std::ranges::any_of(current_entries_, [&prefix](const auto& value) {
            return value.first.starts_with(prefix);
        });
    }

    auto deleted_path(std::string_view path) const -> bool {
        auto lock = std::scoped_lock{overlay_mutex_};
        return std::ranges::any_of(deleted_entries_, [&path](const auto& value) {
            return path_matches(path, value.first, value.second);
        });
    }

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
    observation_sink sink_;
    std::optional<digest> snapshot_root_;
    std::map<std::string, node_kind, std::less<>> virtual_entries_;
    mutable std::mutex overlay_mutex_;
    std::map<std::string, bool, std::less<>> current_entries_;
    std::map<std::string, bool, std::less<>> deleted_entries_;
    std::atomic_uint64_t writes_captured_{};
    mutable std::mutex modification_mutex_;
    std::map<std::string, std::int64_t, std::less<>> modification_times_;
    std::int64_t next_modification_time_{2'000'000'001};
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

auto current_pid() -> std::int64_t {
    const auto* context = fuse_get_context();
    return context ? static_cast<std::int64_t>(context->pid) : 0;
}

runtime::task<int> getattr_request(fuse_backend* state,
                                   std::string path,
                                   std::int64_t pid,
                                   std::shared_ptr<struct stat> output) {
    auto entry = co_await state->entry(path);
    if (!entry)
        co_return fuse_error(entry.error());
    std::memset(output.get(), 0, sizeof(struct stat));
    output->st_mode = static_cast<mode_t>(entry->mode);
    output->st_nlink = entry->kind == node_kind::blob ? 1 : 2;
    output->st_size = static_cast<off_t>(entry->size);
    output->st_uid = getuid();
    output->st_gid = getgid();
#if defined(__APPLE__)
    output->st_mtimespec.tv_sec = state->modification_time(path);
#else
    output->st_mtim.tv_sec = state->modification_time(path);
#endif
    state->observe(observed_operation::getattr, std::move(path), pid, entry->object);
    co_return 0;
}

runtime::task<int> readdir_request(fuse_backend* state,
                                   std::string path,
                                   std::int64_t pid,
                                   std::shared_ptr<std::vector<std::string>> output) {
    auto entries = co_await state->children(path);
    if (!entries)
        co_return fuse_error(entries.error());
    output->reserve(entries->size() + 2);
    output->push_back(".");
    output->push_back("..");
    for (const auto& entry : *entries)
        output->push_back(entry.name);
    state->observe(observed_operation::readdir, std::move(path), pid);
    co_return 0;
}

runtime::task<int>
open_request(fuse_backend* state, std::string path, std::int64_t pid, int flags) {
    auto entry = co_await state->entry(path);
    if (!entry)
        co_return fuse_error(entry.error());
    if (entry->kind != node_kind::blob)
        co_return -EISDIR;
    const auto writing = (flags & (O_WRONLY | O_RDWR)) != 0;
    if ((state->read_only() || state->immutable(path)) && writing) {
        state->observe(observed_operation::write, std::move(path), pid, {}, true);
        co_return -EROFS;
    }
    if (!state->read_only() && !state->immutable(path) && (flags & O_TRUNC) != 0) {
        auto written = co_await state->source().write(path, {}, {.mode = entry->mode});
        if (!written)
            co_return fuse_error(written.error());
        state->mark_current(path);
        state->cache().invalidate(entry->node);
        state->captured();
        state->touch(path);
        state->observe(observed_operation::write, path, pid);
    }
    state->observe(observed_operation::open, std::move(path), pid, entry->object);
    co_return 0;
}

runtime::task<int> read_request(fuse_backend* state,
                                std::string path,
                                std::int64_t pid,
                                std::uint64_t offset,
                                std::size_t size,
                                std::shared_ptr<std::vector<std::byte>> output) {
    auto entry = co_await state->entry(path);
    if (!entry)
        co_return fuse_error(entry.error());
    auto bytes = co_await state->cache().read(state->source(), *entry, offset, size);
    if (!bytes)
        co_return fuse_error(bytes.error(), true);
    *output = std::move(*bytes);
    state->observe(observed_operation::read, std::move(path), pid, entry->object);
    co_return static_cast<int>(output->size());
}

runtime::task<int>
create_request(fuse_backend* state, std::string path, std::int64_t pid, std::uint32_t mode) {
    if (state->read_only() || state->immutable(path)) {
        state->observe(observed_operation::write, std::move(path), pid, {}, true);
        co_return -EROFS;
    }
    const auto file_mode = static_cast<std::uint32_t>(S_IFREG) | (mode & 0777U);
    auto written = co_await state->source().write(path, {}, {.mode = file_mode});
    if (!written)
        co_return fuse_error(written.error());
    state->mark_current(path);
    state->captured();
    state->touch(path);
    state->observe(observed_operation::write, std::move(path), pid);
    co_return 0;
}

runtime::task<int> write_request(fuse_backend* state,
                                 std::string path,
                                 std::int64_t pid,
                                 std::uint64_t offset,
                                 std::vector<std::byte> input) {
    if (state->read_only() || state->immutable(path)) {
        state->observe(observed_operation::write, std::move(path), pid, {}, true);
        co_return -EROFS;
    }
    auto entry = co_await state->entry(path);
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
    auto written =
        co_await state->source().write(path, std::move(*contents), {.mode = entry->mode});
    if (!written)
        co_return fuse_error(written.error());
    state->mark_current(path);
    state->cache().invalidate(entry->node);
    state->captured();
    state->touch(path);
    state->observe(observed_operation::write, std::move(path), pid);
    co_return static_cast<int>(input.size());
}

runtime::task<int>
remove_request(fuse_backend* state, std::string path, std::int64_t pid, bool directory) {
    if (state->read_only() || state->immutable(path))
        co_return -EROFS;
    auto entry = co_await state->entry(path);
    if (!entry)
        co_return fuse_error(entry.error());
    if (directory && entry->kind == node_kind::blob)
        co_return -ENOTDIR;
    if (!directory && entry->kind != node_kind::blob)
        co_return -EISDIR;
    if (directory) {
        auto children = co_await state->children(path);
        if (!children)
            co_return fuse_error(children.error());
        if (!children->empty())
            co_return -ENOTEMPTY;
    }
    auto removed = co_await state->source().remove(path);
    if (!removed)
        co_return fuse_error(removed.error());
    state->mark_deleted(path, directory);
    state->cache().invalidate(entry->node);
    state->captured();
    state->touch(path);
    state->observe(observed_operation::write, std::move(path), pid);
    co_return 0;
}

runtime::task<int>
rename_request(fuse_backend* state, std::string from, std::string to, std::int64_t pid) {
    if (state->read_only() || state->immutable(from) || state->immutable(to))
        co_return -EROFS;
    if (from == to)
        co_return 0;
    auto source = co_await state->entry(from);
    if (!source)
        co_return fuse_error(source.error());
    auto destination = co_await state->entry(to);
    if (destination) {
        if (source->kind == node_kind::blob && destination->kind != node_kind::blob)
            co_return -EISDIR;
        if (source->kind != node_kind::blob && destination->kind == node_kind::blob)
            co_return -ENOTDIR;
        if (destination->kind != node_kind::blob) {
            auto children = co_await state->children(to);
            if (!children)
                co_return fuse_error(children.error());
            if (!children->empty())
                co_return -ENOTEMPTY;
        }
        auto removed = co_await state->source().remove(to);
        if (!removed)
            co_return fuse_error(removed.error());
        state->cache().invalidate(destination->node);
    } else if (destination.error().code != errc::not_found) {
        co_return fuse_error(destination.error());
    }
    auto moved = co_await state->source().move(from, to);
    if (!moved)
        co_return fuse_error(moved.error());
    const auto recursive = source->kind != node_kind::blob;
    state->mark_deleted(from, recursive);
    state->mark_current(to, recursive);
    state->captured();
    state->touch(from);
    state->touch(to);
    state->observe(observed_operation::write, std::move(from), pid);
    state->observe(observed_operation::write, std::move(to), pid);
    co_return 0;
}

runtime::task<int>
mkdir_request(fuse_backend* state, std::string path, std::int64_t pid, std::uint32_t mode) {
    if (state->read_only() || state->immutable(path)) {
        state->observe(observed_operation::write, std::move(path), pid, {}, true);
        co_return -EROFS;
    }
    auto created = co_await repository_detail::vfs_access::make_directory(
        state->source(), path, static_cast<std::uint32_t>(S_IFDIR) | (mode & 0777U));
    if (!created)
        co_return fuse_error(created.error());
    state->mark_current(path, true);
    state->captured();
    state->touch(path);
    state->observe(observed_operation::write, std::move(path), pid);
    co_return 0;
}

runtime::task<int>
truncate_request(fuse_backend* state, std::string path, std::int64_t pid, std::uint64_t size) {
    if (state->read_only() || state->immutable(path)) {
        state->observe(observed_operation::write, std::move(path), pid, {}, true);
        co_return -EROFS;
    }
    auto entry = co_await state->entry(path);
    if (!entry)
        co_return fuse_error(entry.error());
    auto contents = co_await state->cache().read(state->source(), *entry, 0, entry->size);
    if (!contents)
        co_return fuse_error(contents.error(), true);
    contents->resize(static_cast<std::size_t>(size));
    auto written =
        co_await state->source().write(path, std::move(*contents), {.mode = entry->mode});
    if (!written)
        co_return fuse_error(written.error());
    state->mark_current(path);
    state->cache().invalidate(entry->node);
    state->captured();
    state->touch(path);
    state->observe(observed_operation::write, std::move(path), pid);
    co_return 0;
}

runtime::task<int>
chmod_request(fuse_backend* state, std::string path, std::int64_t pid, std::uint32_t mode) {
    if (state->read_only() || state->immutable(path)) {
        state->observe(observed_operation::write, std::move(path), pid, {}, true);
        co_return -EROFS;
    }
    auto entry = co_await state->entry(path);
    if (!entry)
        co_return fuse_error(entry.error());
    const auto type = entry->mode & static_cast<std::uint32_t>(S_IFMT);
    auto changed = co_await repository_detail::vfs_access::set_mode(
        state->source(), path, static_cast<std::uint32_t>(type) | (mode & 0777U));
    if (!changed)
        co_return fuse_error(changed.error());
    state->mark_current(path, entry->kind != node_kind::blob);
    state->captured();
    state->touch(path);
    state->observe(observed_operation::write, std::move(path), pid);
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
    const auto result =
        state->queue()->submit([state, path = fuse_path(path), pid = current_pid(), value] {
            return getattr_request(state, path, pid, value);
        });
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
    const auto result =
        state->queue()->submit([state, path = fuse_path(path), pid = current_pid(), entries] {
            return readdir_request(state, path, pid, entries);
        });
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
    return state->queue()->submit(
        [state, path = fuse_path(path), pid = current_pid(), flags = information->flags] {
            return open_request(state, path, pid, flags);
        });
}

int fuse_read(const char* path, char* buffer, size_t size, off_t offset, struct fuse_file_info*) {
    auto* state = current_backend();
    if (!state || offset < 0)
        return -EIO;
    auto output = std::make_shared<std::vector<std::byte>>();
    const auto result = state->queue()->submit(
        [state, path = fuse_path(path), pid = current_pid(), offset, size, output] {
            return read_request(state, path, pid, static_cast<std::uint64_t>(offset), size, output);
        });
    if (result > 0)
        std::memcpy(buffer, output->data(), static_cast<std::size_t>(result));
    return result;
}

int fuse_create(const char* path, mode_t mode, struct fuse_file_info*) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), pid = current_pid(), mode] {
        return create_request(state, path, pid, static_cast<std::uint32_t>(mode));
    });
}

int fuse_write(
    const char* path, const char* buffer, size_t size, off_t offset, struct fuse_file_info*) {
    auto* state = current_backend();
    if (!state || offset < 0)
        return -EIO;
    auto input = std::vector<std::byte>(reinterpret_cast<const std::byte*>(buffer),
                                        reinterpret_cast<const std::byte*>(buffer + size));
    return state->queue()->submit([state,
                                   path = fuse_path(path),
                                   pid = current_pid(),
                                   offset,
                                   input = std::move(input)]() mutable {
        return write_request(
            state, path, pid, static_cast<std::uint64_t>(offset), std::move(input));
    });
}

int fuse_unlink(const char* path) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), pid = current_pid()] {
        return remove_request(state, path, pid, false);
    });
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
    return state->queue()->submit(
        [state, from = fuse_path(from), to = fuse_path(to), pid = current_pid()] {
            return rename_request(state, from, to, pid);
        });
}

int fuse_mkdir(const char* path, mode_t mode) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), pid = current_pid(), mode] {
        return mkdir_request(state, path, pid, static_cast<std::uint32_t>(mode));
    });
}

int fuse_rmdir(const char* path) {
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), pid = current_pid()] {
        return remove_request(state, path, pid, true);
    });
}

#if defined(LIQUID_HAS_FUSE3)
int fuse_truncate(const char* path, off_t size, struct fuse_file_info*) {
#else
int fuse_truncate(const char* path, off_t size) {
#endif
    auto* state = current_backend();
    if (!state || size < 0)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), pid = current_pid(), size] {
        return truncate_request(state, path, pid, static_cast<std::uint64_t>(size));
    });
}

#if defined(LIQUID_HAS_FUSE3)
int fuse_chmod(const char* path, mode_t mode, struct fuse_file_info*) {
#else
int fuse_chmod(const char* path, mode_t mode) {
#endif
    auto* state = current_backend();
    if (!state)
        return -EIO;
    return state->queue()->submit([state, path = fuse_path(path), pid = current_pid(), mode] {
        return chmod_request(state, path, pid, static_cast<std::uint32_t>(mode));
    });
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
    operations.truncate = fuse_truncate;
    operations.chmod = fuse_chmod;
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

runtime::task<std::shared_ptr<backend>> start_observed_fuse(repository& source,
                                                            digest tree,
                                                            std::filesystem::path mountpoint,
                                                            std::vector<std::string> outputs,
                                                            observation_sink sink,
                                                            bool read_only) {
    auto root = repository_detail::vfs_access::root_path(source);
    if (!root)
        co_return std::unexpected(root.error());
    auto prepared =
        co_await runtime::offload([mountpoint] { return prepare_mountpoint(mountpoint); });
    if (!prepared)
        co_return std::unexpected(prepared.error());
    if (!*prepared)
        co_return std::unexpected(prepared->error());
    auto state = std::make_shared<fuse_backend>(source,
                                                std::move(mountpoint),
                                                std::move(*root),
                                                mount_options{.read_only = read_only},
                                                std::move(sink),
                                                std::move(outputs),
                                                std::move(tree));
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
