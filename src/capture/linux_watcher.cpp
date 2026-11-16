// SPDX-License-Identifier: MIT

#include "capture/watcher.hpp"

#include <poll.h>
#include <sys/inotify.h>
#include <unistd.h>

#include <array>
#include <atomic>
#include <cerrno>
#include <mutex>
#include <system_error>
#include <unordered_map>

#include "liquid/runtime.hpp"

namespace liquid::capture_detail {
namespace {

class inotify_watcher final : public watcher {
public:
    explicit inotify_watcher(std::filesystem::path workspace) : workspace_(std::move(workspace)) {}
    ~inotify_watcher() override {
        stop();
    }

    [[nodiscard]] auto name() const noexcept -> std::string_view override {
        return "inotify";
    }

    auto start(watch_callback callback) -> result<void> override {
        fd_ = inotify_init1(IN_CLOEXEC);
        if (fd_ < 0)
            return std::unexpected(
                error{errc::unavailable, "cannot initialize inotify", workspace_.string()});
        callback_ = std::move(callback);
        auto added = add_tree(workspace_);
        if (!added) {
            ::close(fd_);
            fd_ = -1;
            return std::unexpected(added.error());
        }
        running_ = std::make_shared<std::atomic_bool>(true);
        stopped_ = std::make_shared<std::atomic_bool>(false);
        runtime::spawn(read_loop(running_, fd_, workspace_, callback_, watches_),
                       [stopped = stopped_](result<void>) {
                           stopped->store(true, std::memory_order_release);
                       });
        return {};
    }

    [[nodiscard]] auto stopped() const noexcept -> bool override {
        return stopped_->load(std::memory_order_acquire);
    }

    void stop() noexcept override {
        if (running_)
            running_->store(false, std::memory_order_release);
        if (stopped() && fd_ >= 0) {
            ::close(fd_);
            fd_ = -1;
        }
    }

    void inject_overflow_for_test() override {
        if (callback_)
            callback_({watch_event_kind::reconcile, workspace_, 0});
    }

private:
    auto add_tree(const std::filesystem::path& root) -> result<void> {
        auto add = [this](const std::filesystem::path& directory) -> result<void> {
            const auto descriptor =
                inotify_add_watch(fd_,
                                  directory.c_str(),
                                  IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE |
                                      IN_DELETE | IN_DELETE_SELF | IN_MOVE_SELF);
            if (descriptor < 0)
                return std::unexpected(
                    error{errc::io, "cannot add inotify watch", directory.string()});
            watches_[descriptor] = directory;
            return {};
        };
        auto first = add(root);
        if (!first)
            return std::unexpected(first.error());
        for (std::filesystem::recursive_directory_iterator it(root), end; it != end; ++it) {
            if (it->is_directory() && it->path().filename() != ".liquid") {
                auto added = add(it->path());
                if (!added)
                    return std::unexpected(added.error());
            }
        }
        return {};
    }

    static runtime::task<void> read_loop(std::shared_ptr<std::atomic_bool> running,
                                         int fd,
                                         std::filesystem::path workspace,
                                         watch_callback callback,
                                         std::unordered_map<int, std::filesystem::path> watches) {
        while (running->load(std::memory_order_acquire)) {
            auto bytes = co_await runtime::offload([fd] {
                auto descriptor = pollfd{.fd = fd, .events = POLLIN, .revents = 0};
                const auto ready = ::poll(&descriptor, 1, 50);
                if (ready < 0 && errno != EINTR)
                    throw std::system_error(errno, std::generic_category(), "inotify poll");
                if (ready <= 0)
                    return std::vector<char>{};
                auto buffer = std::array<char, 64 * 1024>{};
                const auto count = ::read(fd, buffer.data(), buffer.size());
                if (count < 0)
                    throw std::system_error(errno, std::generic_category(), "inotify read");
                return std::vector<char>{buffer.begin(), buffer.begin() + count};
            });
            if (!bytes) {
                if (!running->load(std::memory_order_acquire))
                    break;
                co_return std::unexpected(bytes.error());
            }
            if (bytes->empty())
                continue;
            for (std::size_t offset = 0; offset < bytes->size();) {
                const auto* event = reinterpret_cast<const inotify_event*>(bytes->data() + offset);
                if (event->mask & IN_Q_OVERFLOW) {
                    callback({watch_event_kind::reconcile, workspace, 0});
                } else if (const auto found = watches.find(event->wd); found != watches.end()) {
                    const auto path = found->second / std::string{event->name};
                    if ((event->mask & IN_ISDIR) && (event->mask & (IN_CREATE | IN_MOVED_TO))) {
                        const auto descriptor = inotify_add_watch(
                            fd,
                            path.c_str(),
                            IN_CLOSE_WRITE | IN_MOVED_TO | IN_MOVED_FROM | IN_CREATE | IN_DELETE |
                                IN_DELETE_SELF | IN_MOVE_SELF);
                        if (descriptor >= 0)
                            watches[descriptor] = path;
                        else
                            callback({watch_event_kind::reconcile, workspace, 0});
                    }
                    if ((event->mask & IN_ISDIR) && (event->mask & (IN_MOVED_FROM | IN_DELETE)))
                        callback({watch_event_kind::reconcile, workspace, 0});
                    if (event->mask & (IN_DELETE | IN_MOVED_FROM))
                        callback({watch_event_kind::remove, path, 0});
                    else
                        callback({watch_event_kind::write, path, 0});
                }
                offset += sizeof(inotify_event) + event->len;
            }
        }
        co_return {};
    }

    std::filesystem::path workspace_;
    watch_callback callback_;
    int fd_{-1};
    std::shared_ptr<std::atomic_bool> running_;
    std::shared_ptr<std::atomic_bool> stopped_{std::make_shared<std::atomic_bool>(true)};
    std::unordered_map<int, std::filesystem::path> watches_;
};

} // namespace

auto make_watcher(const std::filesystem::path& workspace, std::chrono::milliseconds)
    -> std::unique_ptr<watcher> {
    return std::make_unique<inotify_watcher>(workspace);
}

} // namespace liquid::capture_detail
