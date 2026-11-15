// SPDX-License-Identifier: MIT

#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/drip.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/remote.hpp"
#include "liquid/runtime.hpp"
#include "liquid/session.hpp"

namespace liquid {

class repository;
class store;

enum class sync_item_kind : std::uint8_t {
    drip = 1,
    change = 2,
    directory = 3,
    reference = 4,
    presence = 5,
    // An author's identity document chain (`body` is `encode_identity_chain`). It travels ahead of
    // the drips and changes it attributes; the receiver verifies and stores it before them.
    identity = 6,
};

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

struct LIQUID_EXPORT sync_metadata_report {
    std::uint64_t trees{};
    std::uint64_t blobs{};
    std::uint64_t bytes_remote{};
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
    std::uint64_t bytes_remote{};
    std::vector<sync_ref_outcome> references;
    std::uint64_t retry_after_ms{};
    std::string exhausted_scope;
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

// Runs before the pushed records are imported, so `sync_item::author` is still the name the frame
// claims, not a verified identity. Authorize on the authenticated caller; the import that follows
// refuses any drip or change whose author no identity chain verifies (liquid#429).
using sync_authorizer = std::function<runtime::task<std::vector<sync_ref_verdict>>(
    const sync_item&, std::span<const sync_ref_update> reference_updates)>;

// The name the push rate limit is charged to when the host did not say who authenticated the
// push. Every unauthenticated push shares this one actor bucket (liquid#508).
inline constexpr std::string_view unauthenticated_pusher = "unauthenticated";

struct LIQUID_EXPORT sync_server_options {
    sync_authorizer authorize;
    std::size_t batch_items{256};
    // The identity that authenticated the push: the session or TAAP identity the host verified
    // (Lake's signed-in caller, for example). The push rate limit's actor bucket is keyed on it,
    // never on a record's claimed `sync_item::author`, so a pusher cannot spend another person's
    // budget by naming them (liquid#508). Empty means `unauthenticated_pusher`.
    std::string pusher;
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

    // Charges the push rate limit to `sync_server_options::pusher`.
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    handle_push(std::span<const std::byte> request);
    // Charges the push rate limit to `pusher`, the identity that authenticated this request, for a
    // server that handles requests from more than one caller. Empty means `unauthenticated_pusher`.
    [[nodiscard]] runtime::task<std::vector<std::byte>>
    handle_push(std::span<const std::byte> request, std::string pusher);
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
    sync_session(store& local, remote_blob_source& peer, std::string source, std::string instance);
    ~sync_session();
    sync_session(sync_session&&) noexcept;
    sync_session& operator=(sync_session&&) noexcept;
    sync_session(const sync_session&) = delete;
    sync_session& operator=(const sync_session&) = delete;

    [[nodiscard]] runtime::task<sync_transfer_report> send(std::span<const digest> roots);
    [[nodiscard]] runtime::task<sync_transfer_report> receive(std::span<const digest> roots);
    [[nodiscard]] runtime::task<sync_metadata_report>
    receive_metadata(std::span<const digest> roots);

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
    [[nodiscard]] runtime::task<sync_report> pull_metadata(repository& destination);

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
