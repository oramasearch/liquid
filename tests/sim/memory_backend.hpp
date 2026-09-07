#pragma once

#include <chrono>
#include <memory>
#include <optional>

#include "liquid/store.hpp"

namespace liquid::sim {

class memory_store_backend final : public store_backend {
public:
    explicit memory_store_backend(std::chrono::milliseconds latency = {});

    void set_latency(std::chrono::milliseconds latency) noexcept;
    void fail_next(error failure);

    [[nodiscard]] runtime::task<void> put(std::string key, std::vector<std::byte> bytes) override;
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
};

} // namespace liquid::sim
