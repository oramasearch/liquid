// SPDX-License-Identifier: MIT

#pragma once

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <memory>

#include "liquid/store.hpp"

namespace liquid::sim {

// A store backend with the range contract of Zetta's object storage (zetta#1005, liquid #332),
// decorating another backend. put_range only appends at the object's current end, an absent
// object being empty. A retry that resends bytes already held succeeds and changes nothing, and
// any other range, one that would rewrite different bytes or leave a gap, fails errc::conflict
// without reaching the decorated backend. Complete-object put, put_if_absent and remove are
// unchanged. fenced() decorates the decorated backend's fenced view the same way, so a store held
// by an injected owner keeps the contract. The decorated backend's local root is not visible
// through it, so a store over a disk backend reads its index runs as a remote store does.
class write_once_store_backend final : public store_backend {
public:
    explicit write_once_store_backend(std::shared_ptr<store_backend> inner);
    write_once_store_backend(std::shared_ptr<store_backend> inner,
                             std::shared_ptr<std::atomic<std::uint64_t>> refused);

    // Ranges refused with errc::conflict since construction.
    [[nodiscard]] std::uint64_t refused() const noexcept;

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
    [[nodiscard]] result<std::shared_ptr<store_backend>> fenced(std::uint64_t fence) const override;

private:
    std::shared_ptr<store_backend> inner_;
    std::shared_ptr<std::atomic<std::uint64_t>> refused_;
};

// The environment variable that sends the store, garbage-collection and index suites through a
// write_once_store_backend over the disk backend. CMake registers each suite a second time with it.
inline constexpr const char* write_once_environment = "LIQUID_TEST_WRITE_ONCE_BACKEND";

[[nodiscard]] bool write_once_requested() noexcept;

// The backend a disk-rooted test store opens with: null (the default disk backend) normally, and a
// write_once_store_backend over the disk backend at `root` when write_once_requested().
[[nodiscard]] std::shared_ptr<store_backend> test_store_backend(const std::filesystem::path& root,
                                                                bool durable = false);

} // namespace liquid::sim
