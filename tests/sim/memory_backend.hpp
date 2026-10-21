#pragma once

#include <chrono>
#include <cstddef>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/store.hpp"

namespace liquid::sim {

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
    // How many mutations a crash now would lose.
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
