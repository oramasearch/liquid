#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/store.hpp"

namespace liquid::sim {

// What a crash keeps of the mutations issued since the last successful barrier (liquid #344).
// store_backend's ordering contract (include/liquid/store.hpp) is `subset`; the stronger modes are
// what particular backends give and liquid may not rely on.
enum class crash_durability : std::uint8_t {
    // Every issued mutation survives: a crash of the process, not of the backend.
    everything,
    // None survives: a backend that keeps only what a barrier covered.
    barriered,
    // A prefix of the issue order survives, as from a backend with one ordered log.
    prefix,
    // Any subset survives, except that each key keeps a prefix of its own mutations: a backend
    // that sends different keys' writes concurrently, or keeps them in different logs.
    subset,
};

class memory_store_backend final : public store_backend {
public:
    explicit memory_store_backend(std::chrono::milliseconds latency = {});

    void set_latency(std::chrono::milliseconds latency) noexcept;
    void fail_next(error failure);
    // Every key the backend holds, for tests that prove nothing was written elsewhere.
    [[nodiscard]] std::vector<std::string> keys() const;
    // While frozen every mutation fails errc::unavailable and nothing changes. A crash simulation
    // freezes the backend at the crash point, so work the crashed process would never have done
    // (destructors, cleanup) cannot reach the state its successor recovers.
    void set_frozen(bool frozen) noexcept;
    // Crash durability (liquid #343): only what a barrier has covered is durable. Drops every
    // mutation made since the last successful barrier, as a crash of a backend that keeps nothing
    // unbarriered would. Losing all of them is a prefix of any issue order (the empty one), so it
    // holds under every ordering contract between barriers (liquid #344).
    void lose_unbarriered();
    // A crash of the process using this backend (liquid #344): what was barriered stays, and of the
    // mutations since the last successful barrier the backend keeps what `durability` allows,
    // chosen deterministically from `seed`. Returns how many of them survived. Call it with the
    // backend not frozen, after the crashed process's handles are gone.
    std::size_t crash(crash_durability durability, std::uint64_t seed);
    // How many mutations a crash now could lose.
    [[nodiscard]] std::size_t unbarriered() const;

    [[nodiscard]] runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override;
    [[nodiscard]] runtime::task<void> put_if_absent(std::string key,
                                                    std::vector<std::byte> bytes) override;
    [[nodiscard]] runtime::task<void>
    put_range(std::string key, std::uint64_t offset, std::vector<std::byte> bytes) override;
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    get_range(std::string key, std::uint64_t offset, std::uint64_t size) override;
    [[nodiscard]] runtime::task<void> remove(std::string key) override;
    [[nodiscard]] runtime::task<std::vector<store_backend_entry>> list(std::string prefix) override;
    [[nodiscard]] runtime::task<void> barrier() override;

private:
    struct state;
    std::shared_ptr<state> state_;

    [[nodiscard]] runtime::task<void> wait();
    [[nodiscard]] std::optional<error> take_failure();
    [[nodiscard]] std::optional<error> take_mutation_failure(const std::string& key);
};

} // namespace liquid::sim
