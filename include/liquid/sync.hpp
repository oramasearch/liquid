#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "liquid/drip.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/remote.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;
class store;

enum class sync_item_kind : std::uint8_t { drip = 1, change = 2, directory = 3, reference = 4 };

struct LIQUID_EXPORT sync_item {
    sync_item_kind kind{sync_item_kind::drip};
    actor author;
    node_id repository{};
    node_id directory{};
    bool root{};
    std::string name;
    change_id reference{};
    std::vector<std::byte> body;
    std::vector<std::byte> signature;
};

struct LIQUID_EXPORT sync_export {
    journal_cursor cursor;
    std::size_t count{};
    bool done{};
    std::vector<digest> objects;
};

struct LIQUID_EXPORT sync_import_report {
    std::size_t items{};
};

struct LIQUID_EXPORT sync_transfer_report {
    std::uint64_t objects{};
    std::uint64_t bytes{};
};

enum class sync_ref_outcome_kind : std::uint8_t { applied = 1, refused = 2 };

struct LIQUID_EXPORT sync_ref_outcome {
    std::string name;
    sync_ref_outcome_kind outcome{sync_ref_outcome_kind::applied};
    std::string reason;
};

struct LIQUID_EXPORT sync_report {
    journal_cursor cursor;
    std::uint64_t metadata_items{};
    sync_transfer_report sent;
    sync_transfer_report received;
    std::vector<sync_ref_outcome> references;
};

struct LIQUID_EXPORT sync_endpoint_options {
    std::string bearer_token;
    journal_cursor cursor;
    std::size_t batch_items{256};
    bool atomic{};
};

struct LIQUID_EXPORT sync_ref_update {
    std::string name;
    std::optional<change_id> old_head;
    change_id new_head;
    bool fast_forward{};
};

enum class sync_ref_verdict_kind : std::uint8_t { accepted = 1, refused = 2 };

struct LIQUID_EXPORT sync_ref_verdict {
    std::string name;
    sync_ref_verdict_kind verdict{sync_ref_verdict_kind::accepted};
    std::string reason;
};

using sync_authorizer = std::function<runtime::task<std::vector<sync_ref_verdict>>(
    const sync_item&, std::span<const sync_ref_update> reference_updates)>;

struct LIQUID_EXPORT sync_server_options {
    sync_authorizer authorize;
    std::size_t batch_items{256};
};

class LIQUID_EXPORT sync_server {
public:
    explicit sync_server(repository& subject, sync_server_options options = {});
    sync_server(repository& subject, store& objects, sync_server_options options = {});
    ~sync_server();
    sync_server(sync_server&&) noexcept;
    sync_server& operator=(sync_server&&) noexcept;
    sync_server(const sync_server&) = delete;
    sync_server& operator=(const sync_server&) = delete;

    [[nodiscard]] runtime::task<std::vector<std::byte>>
    handle_push(std::span<const std::byte> request);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    handle_pull(std::span<const std::byte> request);
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    handle_find_missing(std::span<const std::byte> request);
    [[nodiscard]] runtime::task<void> handle_chunks(std::span<const std::byte> request);
    [[nodiscard]] runtime::task<std::uint64_t>
    handle_chunks(digest id, std::uint64_t offset, std::span<std::byte> output);
    [[nodiscard]] static result<std::vector<std::byte>> encode_error(const error& value);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT sync_session {
public:
    sync_session(store& local, remote_blob_source& peer, std::string instance);
    ~sync_session();
    sync_session(sync_session&&) noexcept;
    sync_session& operator=(sync_session&&) noexcept;
    sync_session(const sync_session&) = delete;
    sync_session& operator=(const sync_session&) = delete;

    [[nodiscard]] runtime::task<sync_transfer_report> send(std::span<const digest> roots);
    [[nodiscard]] runtime::task<sync_transfer_report> receive(std::span<const digest> roots);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

class LIQUID_EXPORT sync_endpoint final : public remote_blob_source {
public:
    explicit sync_endpoint(std::string repository_url, sync_endpoint_options options = {});
    ~sync_endpoint() override;
    sync_endpoint(sync_endpoint&&) noexcept;
    sync_endpoint& operator=(sync_endpoint&&) noexcept;
    sync_endpoint(const sync_endpoint&) = delete;
    sync_endpoint& operator=(const sync_endpoint&) = delete;

    [[nodiscard]] runtime::task<sync_report> push(repository& source);
    [[nodiscard]] runtime::task<sync_report> pull(repository& destination);

    [[nodiscard]] runtime::task<std::vector<digest>>
    find_missing(std::string instance, std::vector<digest> blobs) override;
    [[nodiscard]] runtime::task<std::uint64_t> read_range(std::string instance,
                                                          digest blob,
                                                          std::uint64_t offset,
                                                          std::span<std::byte> output) override;
    [[nodiscard]] runtime::task<void>
    write(std::string instance, digest blob, std::vector<std::byte> contents) override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
