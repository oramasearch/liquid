// SPDX-License-Identifier: MIT

#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/data.hpp"
#include "liquid/drip.hpp"
#include "liquid/export.hpp"
#include "liquid/frontend.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT write_options {
    std::uint32_t mode{0100644};
    std::optional<write_base> base;
    std::vector<std::string> depends_on;
    enum class stale_action : std::uint8_t {
        refuse,
        accept_if_disjoint,
        force,
    } on_stale{stale_action::refuse};
    std::optional<frontend_source_range> range;
    std::optional<std::string> symbol;
};

enum class lease_kind : std::uint8_t {
    file,
    symbol,
    range,
};

enum class lease_authority : std::uint8_t {
    advisory,
    lake,
};

struct LIQUID_EXPORT lease_claim {
    std::string path;
    lease_kind kind{lease_kind::file};
    std::optional<std::string> symbol;
    std::optional<frontend_source_range> range;
};

struct LIQUID_EXPORT semantic_lease {
    lease_claim claim;
    liquid::actor by;
    session_id session;
    std::chrono::sys_seconds since{};
    std::chrono::sys_seconds expires{};
    lease_authority authority{lease_authority::advisory};
};

using lease_authorizer = std::function<runtime::task<void>(semantic_lease requested)>;

struct LIQUID_EXPORT lock_rule {
    std::string name;
    std::vector<std::string> paths;

    auto operator==(const lock_rule&) const -> bool = default;
};

struct LIQUID_EXPORT lock_config {
    std::vector<lock_rule> rules;

    auto operator==(const lock_config&) const -> bool = default;
};

enum class lock_authority : std::uint8_t {
    advisory,
    lake,
};

struct LIQUID_EXPORT exclusive_lock {
    std::string path;
    liquid::actor holder;
    std::chrono::sys_seconds since{};
    std::string reason;
    lock_authority authority{lock_authority::advisory};

    auto operator==(const exclusive_lock& other) const -> bool {
        return path == other.path && holder.taap_id == other.holder.taap_id &&
               holder.key_fingerprint == other.holder.key_fingerprint && since == other.since &&
               reason == other.reason && authority == other.authority;
    }
};

enum class lock_operation : std::uint8_t {
    acquire,
    release,
};

struct LIQUID_EXPORT lock_authorization_request {
    lock_operation operation{lock_operation::acquire};
    exclusive_lock value;
};

using lock_authorizer = std::function<runtime::task<void>(lock_authorization_request requested)>;

enum class presence_activity : std::uint8_t {
    editing,
    reading,
    idle,
};

struct LIQUID_EXPORT presence {
    liquid::actor actor;
    session_id session;
    std::string path;
    std::optional<frontend_source_range> range;
    presence_activity activity{presence_activity::idle};
    std::chrono::sys_seconds at{};
};

struct LIQUID_EXPORT line_attribution {
    std::uint64_t line{};
    std::string text;
    liquid::actor by;
    session_id session;
    change_id change;
    bool agent{};
    // Set from the change or operation the line came from. Otherwise show
    // `unverified_signer_label`, never `by` (liquid#508).
    bool identity_verified{};
};

struct LIQUID_EXPORT session_options {
    liquid::actor actor;
    attestation delegation;
};

struct LIQUID_EXPORT publish_options {
    std::optional<std::string> message;
    bool atomic{true};
};

struct LIQUID_EXPORT publish_path_result {
    std::string path;
    bool applied{};
    std::string reason;
};

struct LIQUID_EXPORT publish_report {
    session_id session;
    std::vector<publish_path_result> paths;
    std::uint64_t directories{};
    std::optional<change_id> change;
};

struct LIQUID_EXPORT epoch_options {
    std::uint64_t min_ack_peers{};
};

struct LIQUID_EXPORT epoch {
    std::uint64_t n{};
    std::vector<document_heads> heads;
    std::chrono::sys_seconds at{};
    std::uint64_t document_bytes{};
};

using epoch_acknowledger = std::function<runtime::task<std::uint64_t>(const epoch& requested)>;

class LIQUID_EXPORT session {
public:
    session() noexcept;
    ~session();
    session(session&&) noexcept;
    auto operator=(session&&) noexcept -> session&;
    session(const session&) = delete;
    auto operator=(const session&) -> session& = delete;

    [[nodiscard]] session_id id() const noexcept;
    [[nodiscard]] runtime::task<std::vector<std::byte>> read(std::string path);
    [[nodiscard]] runtime::task<void> write(std::string path, std::vector<std::byte> contents);
    [[nodiscard]] runtime::task<void>
    write(std::string path, std::vector<std::byte> contents, write_options options);
    [[nodiscard]] runtime::task<void>
    write_file(std::string path, std::filesystem::path source, write_options options = {});
    [[nodiscard]] runtime::task<void>
    move(std::string from, std::string to, write_options options = {});
    [[nodiscard]] runtime::task<void> remove(std::string path, write_options options = {});
    [[nodiscard]] runtime::task<void> lease(lease_claim claim, std::chrono::seconds ttl);
    [[nodiscard]] runtime::task<void> release(lease_claim claim);
    [[nodiscard]] runtime::task<void> report_presence(presence value);
    [[nodiscard]] runtime::task<publish_report> publish(publish_options options = {});
    [[nodiscard]] runtime::task<void> discard();

private:
    struct impl;
    explicit session(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
    friend class repository;
};

} // namespace liquid
