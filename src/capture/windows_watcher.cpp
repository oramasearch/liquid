// SPDX-License-Identifier: MIT

#include "capture/watcher.hpp"

#include <windows.h>

#include <array>
#include <atomic>
#include <cstddef>
#include <memory>
#include <system_error>

#include "liquid/runtime.hpp"

namespace liquid::capture_detail {
namespace {

class directory_watcher final : public watcher {
public:
    explicit directory_watcher(std::filesystem::path workspace)
        : workspace_(std::move(workspace)) {}
    ~directory_watcher() override {
        stop();
    }

    [[nodiscard]] auto name() const noexcept -> std::string_view override {
        return "ReadDirectoryChangesW";
    }

    auto start(watch_callback callback) -> result<void> override {
        handle_ = CreateFileW(workspace_.c_str(),
                              FILE_LIST_DIRECTORY,
                              FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr,
                              OPEN_EXISTING,
                              FILE_FLAG_BACKUP_SEMANTICS,
                              nullptr);
        if (handle_ == INVALID_HANDLE_VALUE)
            return std::unexpected(
                error{errc::unavailable, "cannot open workspace watcher", workspace_.string()});
        running_ = std::make_shared<std::atomic_bool>(true);
        stopped_ = std::make_shared<std::atomic_bool>(false);
        callback_ = std::move(callback);
        runtime::spawn(read_loop(running_, handle_, workspace_, callback_),
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
        if (handle_ != INVALID_HANDLE_VALUE) {
            CancelIoEx(handle_, nullptr);
            CloseHandle(handle_);
            handle_ = INVALID_HANDLE_VALUE;
        }
    }

    void inject_overflow_for_test() override {
        if (callback_)
            callback_({watch_event_kind::reconcile, workspace_, 0});
    }

private:
    static runtime::task<void> read_loop(std::shared_ptr<std::atomic_bool> running,
                                         HANDLE handle,
                                         std::filesystem::path workspace,
                                         watch_callback callback) {
        while (running->load(std::memory_order_acquire)) {
            auto result = co_await runtime::offload([handle] {
                auto buffer = std::array<std::byte, 64 * 1024>{};
                auto bytes = DWORD{};
                const auto ok = ReadDirectoryChangesW(
                    handle,
                    buffer.data(),
                    static_cast<DWORD>(buffer.size()),
                    TRUE,
                    FILE_NOTIFY_CHANGE_FILE_NAME | FILE_NOTIFY_CHANGE_DIR_NAME |
                        FILE_NOTIFY_CHANGE_SIZE | FILE_NOTIFY_CHANGE_LAST_WRITE,
                    &bytes,
                    nullptr,
                    nullptr);
                if (!ok)
                    throw std::system_error(static_cast<int>(GetLastError()),
                                            std::system_category(),
                                            "ReadDirectoryChangesW");
                return std::pair{buffer, bytes};
            });
            if (!result) {
                if (!running->load(std::memory_order_acquire))
                    break;
                co_return std::unexpected(result.error());
            }
            const auto& [buffer, bytes] = *result;
            if (bytes == 0) {
                callback({watch_event_kind::reconcile, workspace, 0});
                continue;
            }
            for (auto offset = DWORD{0}; offset < bytes;) {
                const auto* event =
                    reinterpret_cast<const FILE_NOTIFY_INFORMATION*>(buffer.data() + offset);
                const auto name =
                    std::wstring{event->FileName, event->FileNameLength / sizeof(wchar_t)};
                const auto path = workspace / std::filesystem::path{name};
                const auto kind = event->Action == FILE_ACTION_REMOVED ||
                                          event->Action == FILE_ACTION_RENAMED_OLD_NAME
                                      ? watch_event_kind::remove
                                      : watch_event_kind::write;
                callback({kind, path, 0});
                if (event->NextEntryOffset == 0)
                    break;
                offset += event->NextEntryOffset;
            }
        }
        co_return {};
    }

    std::filesystem::path workspace_;
    watch_callback callback_;
    HANDLE handle_{INVALID_HANDLE_VALUE};
    std::shared_ptr<std::atomic_bool> running_;
    std::shared_ptr<std::atomic_bool> stopped_{std::make_shared<std::atomic_bool>(true)};
};

} // namespace

auto make_watcher(const std::filesystem::path& workspace, std::chrono::milliseconds)
    -> std::unique_ptr<watcher> {
    return std::make_unique<directory_watcher>(workspace);
}

} // namespace liquid::capture_detail
