#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"
#include "liquid/sync.hpp"

namespace liquid {

enum class drive_reconciliation_kind : std::uint8_t {
    local_only,
    remote_only,
    content_conflict,
    moved,
    delete_edit,
    interrupted_transfer,
};

struct LIQUID_EXPORT drive_reconciliation {
    std::string path;
    std::optional<std::string> old_path;
    drive_reconciliation_kind kind{drive_reconciliation_kind::local_only};
    std::vector<digest> values;
};

struct LIQUID_EXPORT drive_tree_state {
    std::optional<digest> remote;
    std::optional<digest> local;
    std::optional<digest> base;
    journal_cursor remote_cursor;
    bool interrupted{};
};

struct LIQUID_EXPORT drive_status {
    std::filesystem::path folder;
    std::string remote;
    drive_tree_state trees;
    std::uint64_t files{};
    std::uint64_t state_bytes{};
    std::uint64_t state_memory_ceiling{};
    std::uint64_t full_walks{};
    std::vector<conflict> conflicts;
    std::vector<sync_ref_outcome> references;
};

struct LIQUID_EXPORT drive_sync_report {
    drive_status status;
    std::vector<drive_reconciliation> paths;
    sync_transfer_report sent;
    sync_transfer_report received;
};

struct LIQUID_EXPORT drive_options {
    std::string bearer_token;
    std::size_t batch_items{256};
    std::uint64_t state_memory_ceiling{256ULL * 1024ULL * 1024ULL};
    bool durable{true};
};

class LIQUID_EXPORT drive_client {
public:
    drive_client() noexcept;
    ~drive_client();
    drive_client(drive_client&&) noexcept;
    drive_client& operator=(drive_client&&) noexcept;
    drive_client(const drive_client&) = delete;
    drive_client& operator=(const drive_client&) = delete;

    [[nodiscard]] static runtime::task<drive_client>
    open(std::filesystem::path folder, std::string remote, drive_options options = {});
    [[nodiscard]] static runtime::task<drive_client> open(std::filesystem::path folder,
                                                          drive_options options = {});
    [[nodiscard]] runtime::task<drive_sync_report> sync();
    [[nodiscard]] runtime::task<drive_status> status();

private:
    [[nodiscard]] static runtime::task<drive_client> open_impl(std::filesystem::path folder,
                                                               std::optional<std::string> remote,
                                                               drive_options options);
    struct impl;
    explicit drive_client(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
