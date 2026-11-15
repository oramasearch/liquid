#pragma once

#include <array>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <memory>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/x509.hpp"

namespace liquid::transparency {

struct timestamp_verification {
    std::chrono::sys_seconds generated_at;
    std::string policy;
    std::string nonce;
    std::string digest_algorithm;
    std::string signature_algorithm;
    x509::certificate signer;
    std::vector<x509::certificate> certificate_path;
};

class LIQUID_EXPORT timestamp_token {
public:
    static auto verify(std::span<const std::byte> token,
                       std::span<const std::byte> imprint_value,
                       const x509::trust_anchor_set& trust) -> result<timestamp_verification>;
};

struct archive_timestamp_verification {
    std::uint64_t sequence = 0;
    std::vector<std::byte> encoded;
    timestamp_verification timestamp;
};

struct evidence_verification {
    std::string digest_algorithm;
    std::vector<archive_timestamp_verification> archive_timestamps;
};

class LIQUID_EXPORT evidence_record {
public:
    static auto verify(std::span<const std::byte> record,
                       std::span<const std::byte> checkpoint_signature_value,
                       const x509::trust_anchor_set& trust) -> result<evidence_verification>;
};

inline constexpr std::size_t tile_width = 1024;

using hash = std::array<std::byte, 32>;

struct LIQUID_EXPORT witness {
    std::string endpoint;
    std::array<std::byte, 32> public_key{};
};

struct LIQUID_EXPORT log_configuration {
    std::vector<witness> witnesses;
    std::uint64_t witness_threshold{};
};

/// The trust pin for one transparency log: the log's Ed25519 key, the witnesses whose
/// cosignatures count, and how many distinct ones a checkpoint needs. It comes from the caller's
/// own configuration, never from the log, a repository, or a sync peer (liquid#389).
struct LIQUID_EXPORT log_trust {
    actor identity;
    log_configuration configuration;
};

/// Refuses a pin that cannot be used: a key other than a 32-byte Ed25519 key under a
/// `taap:1:ed25519:` identifier, a witness without an endpoint or with a repeated key, or a
/// threshold larger than the number of pinned witnesses.
[[nodiscard]] LIQUID_EXPORT result<void> validate_log_trust(const log_trust& trust);

/// The file name, under `$LIQUID_HOME/trust/transparency/`, that holds the pin for `log_url`.
/// The URL is validated as `http_log_source::create` does.
[[nodiscard]] LIQUID_EXPORT result<std::string> log_trust_file_name(std::string_view log_url);

/// `$LIQUID_HOME/trust/transparency/<log_trust_file_name(log_url)>`.
[[nodiscard]] LIQUID_EXPORT result<std::filesystem::path> log_trust_path(std::string_view log_url);

/// Parses the line format of a pin file. The `log` line must name `log_url`.
[[nodiscard]] LIQUID_EXPORT result<log_trust> parse_log_trust(std::string_view log_url,
                                                              std::string_view text);

/// Writes `trust` in the pin file's line format, the form `parse_log_trust` reads.
[[nodiscard]] LIQUID_EXPORT result<std::string> format_log_trust(std::string_view log_url,
                                                                 const log_trust& trust);

/// Reads the pin for `log_url` from `$LIQUID_HOME/trust/transparency/`. A missing file is
/// `errc::not_configured`, and its message names the file to create.
[[nodiscard]] LIQUID_EXPORT runtime::task<log_trust> load_log_trust(std::string log_url);

/// Writes a new pin file for `log_url`. An existing pin is never replaced: it is
/// `errc::already_exists`, and the operator removes the file to re-pin.
[[nodiscard]] LIQUID_EXPORT runtime::task<void> save_log_trust(std::string log_url,
                                                               log_trust trust);

struct LIQUID_EXPORT checkpoint_signature {
    std::string signer;
    std::vector<std::byte> bytes;
};

struct LIQUID_EXPORT witness_signature {
    std::string endpoint;
    std::array<std::byte, 32> public_key{};
    std::vector<std::byte> bytes;
};

struct LIQUID_EXPORT checkpoint {
    std::uint64_t sequence{};
    std::uint64_t tree_size{};
    std::uint64_t entry_tile_width{};
    std::uint64_t hash_tile_width{};
    hash root_hash{};
    hash checkpoint_hash{};
    checkpoint_signature signature;
    std::vector<witness_signature> witness_signatures;
    std::vector<std::byte> canonical_body;

    [[nodiscard]] result<void> verify(const log_trust& trust) const;
};

struct LIQUID_EXPORT tile {
    std::uint32_t level{};
    std::uint64_t index{};
    std::uint64_t width{};
    hash sha256{};
    std::vector<std::byte> bytes;

    [[nodiscard]] result<std::vector<std::vector<std::byte>>> entries() const;
    [[nodiscard]] result<std::vector<hash>> hashes() const;
};

struct LIQUID_EXPORT inclusion_proof {
    std::uint64_t leaf_index{};
    std::uint64_t tree_size{};
    hash root_hash{};
    std::vector<hash> audit_path;
    checkpoint bound_checkpoint;

    [[nodiscard]] result<void> verify(std::span<const std::byte> attestation,
                                      const checkpoint& checkpoint) const;
};

struct LIQUID_EXPORT consistency_proof {
    std::uint64_t old_tree_size{};
    std::uint64_t new_tree_size{};
    hash old_root_hash{};
    hash new_root_hash{};
    std::vector<hash> audit_path;
    checkpoint bound_checkpoint;

    [[nodiscard]] result<void> verify(const checkpoint& old_checkpoint,
                                      const checkpoint& new_checkpoint) const;
};

struct LIQUID_EXPORT identity_match {
    std::uint64_t index{};
    std::string profile;
    std::string issuer;
    std::string subject;
    std::vector<std::byte> attestation;
};

struct LIQUID_EXPORT identity_scan {
    checkpoint verified_checkpoint;
    std::vector<identity_match> matches;
};

class LIQUID_EXPORT log_source {
public:
    virtual ~log_source() = default;

    [[nodiscard]] virtual std::string name() const = 0;
    /// What the log says its key and witness policy are. Untrusted: verification never reads
    /// them. Only `liquid log trust` shows them, for an operator to confirm before pinning.
    [[nodiscard]] virtual runtime::task<actor> identity() = 0;
    [[nodiscard]] virtual runtime::task<log_configuration> configuration() = 0;
    [[nodiscard]] virtual runtime::task<checkpoint> current_checkpoint() = 0;
    [[nodiscard]] virtual runtime::task<tile>
    get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width) = 0;
    [[nodiscard]] virtual runtime::task<inclusion_proof>
    get_inclusion_proof(std::uint64_t leaf_index, std::uint64_t tree_size) = 0;
    [[nodiscard]] virtual runtime::task<consistency_proof>
    get_consistency_proof(std::uint64_t old_tree_size, std::uint64_t new_tree_size) = 0;
};

class LIQUID_EXPORT http_log_source final : public log_source {
public:
    /// Validate locally and return invalid_argument without DNS, sockets, or HTTP requests.
    [[nodiscard]] static result<http_log_source> create(std::string url);

    /// Retained for existing callers. Invalid URLs are returned by each request operation.
    explicit http_log_source(std::string url);
    ~http_log_source() override;
    http_log_source(http_log_source&&) noexcept;
    http_log_source& operator=(http_log_source&&) noexcept;
    http_log_source(const http_log_source&) = delete;
    http_log_source& operator=(const http_log_source&) = delete;

    [[nodiscard]] std::string name() const override;
    [[nodiscard]] runtime::task<actor> identity() override;
    [[nodiscard]] runtime::task<log_configuration> configuration() override;
    [[nodiscard]] runtime::task<checkpoint> current_checkpoint() override;
    [[nodiscard]] runtime::task<tile>
    get_tile(std::uint32_t level, std::uint64_t index, std::uint64_t width) override;
    [[nodiscard]] runtime::task<inclusion_proof>
    get_inclusion_proof(std::uint64_t leaf_index, std::uint64_t tree_size) override;
    [[nodiscard]] runtime::task<consistency_proof>
    get_consistency_proof(std::uint64_t old_tree_size, std::uint64_t new_tree_size) override;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

enum class bootstrap_policy : std::uint8_t {
    pinned,
    accept_with_warning,
};

struct LIQUID_EXPORT checkpoint_trust {
    checkpoint value;
    bool bootstrap_warning{};
};

[[nodiscard]] LIQUID_EXPORT runtime::task<checkpoint_trust> trust_checkpoint(
    repository& destination, std::string log_name, checkpoint value, bootstrap_policy policy);

class LIQUID_EXPORT log_client {
public:
    explicit log_client(log_source& source);
    ~log_client();
    log_client(log_client&&) noexcept;
    log_client& operator=(log_client&&) noexcept;
    log_client(const log_client&) = delete;
    log_client& operator=(const log_client&) = delete;

    // Every verifying operation takes the caller's pinned `log_trust`. None reads the log's own
    // identity or configuration endpoints (liquid#389).
    [[nodiscard]] runtime::task<checkpoint> current_checkpoint(const log_trust& trust);
    [[nodiscard]] runtime::task<void> verify_tiles(checkpoint value);
    [[nodiscard]] runtime::task<checkpoint> verify_inclusion(std::vector<std::byte> attestation,
                                                             const log_trust& trust);
    [[nodiscard]] runtime::task<checkpoint_trust>
    verify_inclusion(repository& destination,
                     std::vector<std::byte> attestation,
                     const log_trust& trust,
                     bootstrap_policy policy = bootstrap_policy::accept_with_warning);
    [[nodiscard]] runtime::task<checkpoint_trust>
    verify_consistency(repository& destination,
                       const log_trust& trust,
                       bootstrap_policy policy = bootstrap_policy::accept_with_warning);
    [[nodiscard]] runtime::task<identity_scan> scan(std::string identity, const log_trust& trust);

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid::transparency
