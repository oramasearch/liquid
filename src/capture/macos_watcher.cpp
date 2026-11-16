// SPDX-License-Identifier: MIT

#include "capture/watcher.hpp"

#include <CoreServices/CoreServices.h>
#include <dispatch/dispatch.h>

#include <mutex>

namespace liquid::capture_detail {
namespace {

class fsevents_watcher final : public watcher {
public:
    fsevents_watcher(std::filesystem::path workspace, std::chrono::milliseconds latency)
        : workspace_(std::move(workspace)), latency_(latency) {}

    ~fsevents_watcher() override {
        stop();
    }

    [[nodiscard]] auto name() const noexcept -> std::string_view override {
        return "FSEvents";
    }

    auto start(watch_callback callback) -> result<void> override {
        std::lock_guard lock(mutex_);
        if (stream_)
            return std::unexpected(
                error{errc::already_exists, "watcher is already running", "FSEvents"});
        callback_ = std::move(callback);
        auto paths = CFArrayCreateMutable(kCFAllocatorDefault, 1, &kCFTypeArrayCallBacks);
        const auto path = workspace_.string();
        auto root =
            CFStringCreateWithCString(kCFAllocatorDefault, path.c_str(), kCFStringEncodingUTF8);
        CFArrayAppendValue(paths, root);
        CFRelease(root);
        FSEventStreamContext context{};
        context.info = this;
        queue_ = dispatch_queue_create("live.liquid.capture.fsevents", DISPATCH_QUEUE_SERIAL);
        stream_ = FSEventStreamCreate(kCFAllocatorDefault,
                                      &fsevents_watcher::receive,
                                      &context,
                                      paths,
                                      kFSEventStreamEventIdSinceNow,
                                      0.05,
                                      kFSEventStreamCreateFlagFileEvents |
                                          kFSEventStreamCreateFlagNoDefer |
                                          kFSEventStreamCreateFlagWatchRoot);
        CFRelease(paths);
        if (!stream_) {
            dispatch_release(queue_);
            queue_ = nullptr;
            return std::unexpected(
                error{errc::unavailable, "cannot create FSEvents stream", workspace_.string()});
        }
        FSEventStreamSetDispatchQueue(stream_, queue_);
        if (!FSEventStreamStart(stream_)) {
            FSEventStreamInvalidate(stream_);
            FSEventStreamRelease(stream_);
            stream_ = nullptr;
            dispatch_release(queue_);
            queue_ = nullptr;
            return std::unexpected(
                error{errc::unavailable, "cannot start FSEvents stream", workspace_.string()});
        }
        return {};
    }

    void stop() noexcept override {
        std::lock_guard lock(mutex_);
        if (!stream_)
            return;
        FSEventStreamStop(stream_);
        FSEventStreamInvalidate(stream_);
        FSEventStreamRelease(stream_);
        stream_ = nullptr;
        dispatch_sync_f(queue_, nullptr, [](void*) {});
        dispatch_release(queue_);
        queue_ = nullptr;
    }

    [[nodiscard]] auto stopped() const noexcept -> bool override {
        std::lock_guard lock(mutex_);
        return stream_ == nullptr;
    }

    void inject_overflow_for_test() override {
        if (callback_)
            callback_({watch_event_kind::reconcile, workspace_, 0});
    }

private:
    static void receive(ConstFSEventStreamRef,
                        void* context,
                        std::size_t count,
                        void* paths,
                        const FSEventStreamEventFlags flags[],
                        const FSEventStreamEventId ids[]) {
        auto* self = static_cast<fsevents_watcher*>(context);
        auto* values = static_cast<char**>(paths);
        for (std::size_t index = 0; index < count; ++index) {
            const auto flag = flags[index];
            const auto lost = (flag & (kFSEventStreamEventFlagMustScanSubDirs |
                                       kFSEventStreamEventFlagKernelDropped |
                                       kFSEventStreamEventFlagUserDropped)) != 0;
            const auto changed_root = (flag & kFSEventStreamEventFlagRootChanged) != 0;
            const auto kind =
                (lost || changed_root) ? watch_event_kind::reconcile : watch_event_kind::write;
            self->callback_({kind, std::filesystem::path{values[index]}, ids[index]});
        }
    }

    std::filesystem::path workspace_;
    std::chrono::milliseconds latency_;
    watch_callback callback_;
    mutable std::mutex mutex_;
    FSEventStreamRef stream_{nullptr};
    dispatch_queue_t queue_{nullptr};
};

} // namespace

auto make_watcher(const std::filesystem::path& workspace, std::chrono::milliseconds latency)
    -> std::unique_ptr<watcher> {
    return std::make_unique<fsevents_watcher>(workspace, latency);
}

} // namespace liquid::capture_detail
