#pragma once

#include <chrono>
#include <filesystem>
#include <string>

#include "liquid/runtime.hpp"

namespace liquid::store_detail {

// The cross-process owner lock of one disk store root (liquid #321).
//
// A store keeps its pack append position, object map, chunk index and delta candidates in
// process memory, appends at the offset it remembers, and publishes `packs/objects.meta` and the
// index manifest by overwriting them. Exactly one open handle may therefore own a root at a time.
// The lock is an exclusive advisory lock on `<root>/store.lock` (`flock` on POSIX, `LockFileEx`
// on Windows), held for the lifetime of the open store. The kernel releases it when the owning
// descriptor closes, including when the process is killed, so a successor never waits on a dead
// owner. The descriptor is close-on-exec and not inherited by child processes.
//
// After acquiring, the owner writes its pid, host and start time into the file so a refused or
// waiting opener can name who holds the root.
class owner_lock {
public:
    owner_lock() noexcept = default;
    ~owner_lock();
    owner_lock(owner_lock&& other) noexcept;
    owner_lock& operator=(owner_lock&& other) noexcept;
    owner_lock(const owner_lock&) = delete;
    owner_lock& operator=(const owner_lock&) = delete;

    // Takes the lock on `path`, retrying until `wait` has elapsed. A zero wait makes one attempt.
    // Returns `errc::locked` naming the current holder when the wait expires.
    [[nodiscard]] static runtime::task<owner_lock> acquire(std::filesystem::path path,
                                                           std::chrono::milliseconds wait);

    [[nodiscard]] bool held() const noexcept;
    void release() noexcept;

private:
#if defined(_WIN32)
    void* handle_{nullptr};
#else
    int descriptor_{-1};
#endif
};

// "pid <pid> on host <host> since <UTC time>" for this process: the holder an injected owner is
// asked to record, in the same words the disk owner's refusal uses.
[[nodiscard]] std::string holder_description();

} // namespace liquid::store_detail
