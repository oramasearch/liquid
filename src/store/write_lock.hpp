#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <string>

#include "liquid/error.hpp"
#include "liquid/runtime.hpp"

namespace liquid::store_detail {

// The cross-process write lock of a disk store opened without an owner (liquid #526).
//
// A checkout's store is written by several processes at once: the checkout daemon, the CLI and a
// library client such as an IDE. Each keeps the pack append position, the object map and the
// chunk index in memory. Without a lock they append to one pack at the offsets they remember,
// give two index runs one name, and publish object maps and index manifests that drop each
// other's entries. This lock serializes every mutation across processes instead: an exclusive
// advisory lock on `<root>/store.write.lock` (`flock` on POSIX, `LockFileEx` on Windows), held
// only for the length of one mutation, never for the life of a handle.
//
// The file's first eight bytes are a generation, little-endian. Every holder that changed the
// store adds one before it releases the lock. A handle remembers the generation it last saw; a
// different one on its next acquisition means another writer published, and the handle reloads
// its in-memory state from disk before it mutates. The holder also records its pid, host and
// start time after the generation, so a waiter that gives up can name it.
//
// Each handle opens its own descriptor, so two handles in one process exclude each other too. The
// kernel releases the lock when the descriptor closes, however the process ends.
class write_lock {
public:
    write_lock() noexcept = default;
    ~write_lock();
    write_lock(write_lock&& other) noexcept;
    write_lock& operator=(write_lock&& other) noexcept;
    write_lock(const write_lock&) = delete;
    write_lock& operator=(const write_lock&) = delete;

    // Opens (creating) the lock file without locking it.
    [[nodiscard]] static result<write_lock> open(std::filesystem::path path);

    // Takes the lock, retrying with a doubling 1–50 ms delay until `wait` has elapsed. Returns
    // `errc::busy` naming the holder when it expires.
    [[nodiscard]] runtime::task<void> acquire(std::chrono::milliseconds wait);
    void release() noexcept;

    // Reads the generation. Safe without the lock: the holder writes it with one positioned write.
    [[nodiscard]] result<std::uint64_t> generation() const;
    // Writes the generation. Only the holder calls it.
    [[nodiscard]] result<void> set_generation(std::uint64_t value);

    [[nodiscard]] bool valid() const noexcept;
    [[nodiscard]] const std::filesystem::path& path() const noexcept {
        return path_;
    }

private:
    std::filesystem::path path_;
#if defined(_WIN32)
    void* handle_{nullptr};
#else
    int descriptor_{-1};
#endif
};

} // namespace liquid::store_detail
