#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"

namespace liquid {

namespace repository_detail {
struct build_access;
struct testing;
struct vfs_access;
struct git_access;
struct sync_access;
} // namespace repository_detail

enum class encryption_mode : std::uint8_t { leaf, file };

struct LIQUID_EXPORT encryption_path_rule {
    std::string path;
    std::string key_id;
    encryption_mode mode{encryption_mode::leaf};
    std::string encrypted_regex{".*"};

    auto operator==(const encryption_path_rule&) const -> bool = default;
};

struct LIQUID_EXPORT encryption_config {
    std::vector<encryption_path_rule> paths;
};

struct LIQUID_EXPORT cache_config {
    bool publish{};
};

struct LIQUID_EXPORT verify_config {
    std::uint32_t rate{20};
};

struct LIQUID_EXPORT ref_retention_rule {
    std::string name;
    std::string glob;
    std::optional<std::chrono::seconds> duration{std::chrono::days{30}};

    auto operator==(const ref_retention_rule&) const -> bool = default;
};

struct LIQUID_EXPORT result_retention_rule {
    std::string name;
    std::chrono::seconds base{std::chrono::days{30}};

    auto operator==(const result_retention_rule&) const -> bool = default;
};

struct LIQUID_EXPORT size_cap_retention_rule {
    std::string name;
    std::uint64_t bytes{};

    auto operator==(const size_cap_retention_rule&) const -> bool = default;
};

using retention_rule =
    std::variant<ref_retention_rule, result_retention_rule, size_cap_retention_rule>;

struct LIQUID_EXPORT retention_config {
    std::vector<retention_rule> rules{
        ref_retention_rule{"default refs", "*", std::chrono::days{30}},
        result_retention_rule{"default results", std::chrono::days{30}}};

    auto operator==(const retention_config&) const -> bool = default;
};

struct LIQUID_EXPORT retention_explanation {
    digest id{};
    bool retained{};
    std::string reason;
};

struct LIQUID_EXPORT repository_options {
    std::uint64_t checkpoint_interval{64};
    ::liquid::store* store{};
    store_options storage{};
    encryption_config encrypt{};
    cache_config cache{};
    verify_config verify{};
    policy_config policy{};
    retention_config retention{};
    std::optional<remote_config> remote;
};

struct LIQUID_EXPORT log_options {
    std::optional<change_id> from;
    bool first_parent{};
};

struct LIQUID_EXPORT change_input {
    std::vector<change_ref> parents;
    actor author;
    std::chrono::sys_seconds at;
    std::string message;
    std::map<std::string, std::string> metadata;
    std::optional<std::string> ref;
};

struct LIQUID_EXPORT write_options {
    std::uint32_t mode{0100644};
};

struct LIQUID_EXPORT ref {
    std::string name;
    change_id id;
};

struct LIQUID_EXPORT repository_status {
    std::uint64_t pending_drips{};
    std::uint64_t remote_bytes{};
    bool paused{};
    std::vector<conflict> conflicts;
    std::vector<std::string> oversized_directories;
    std::optional<change_id> head;
};

struct LIQUID_EXPORT snapshot {
    change_id id;
    tree root;
};

struct LIQUID_EXPORT key_readability {
    std::string key_id;
    std::uint64_t version{};
    bool readable{};
};

struct LIQUID_EXPORT revision_readability {
    change_id revision;
    std::vector<key_readability> keys;
};

struct LIQUID_EXPORT readability_report {
    std::vector<revision_readability> revisions;
    std::vector<std::string> limits;
};

struct LIQUID_EXPORT op_id {
    digest value;
};

struct LIQUID_EXPORT operation_record {
    op_id id;
    std::string kind;
    std::string from;
    std::string to;
    actor by;
    std::chrono::sys_seconds at;
};

class LIQUID_EXPORT repository {
public:
    repository() noexcept;
    ~repository();
    repository(repository&&) noexcept;
    repository& operator=(repository&&) noexcept;
    repository(const repository&) = delete;
    repository& operator=(const repository&) = delete;

    [[nodiscard]] static runtime::task<repository> create(std::filesystem::path path,
                                                          repository_options options = {});
    [[nodiscard]] static runtime::task<repository> open(std::filesystem::path path,
                                                        repository_options options = {});
    [[nodiscard]] runtime::task<tree> root();
    [[nodiscard]] runtime::task<change_id> head();
    [[nodiscard]] runtime::task<std::vector<std::byte>> read(std::string path);
    [[nodiscard]] runtime::task<std::vector<std::byte>> read_decrypted(std::string path);
    [[nodiscard]] runtime::task<void> write(std::string path, std::vector<std::byte> contents);
    [[nodiscard]] runtime::task<void> write_file(std::string path, std::filesystem::path source);
    [[nodiscard]] runtime::task<void>
    write_remote(std::string path, blob value, write_options options = {});
    [[nodiscard]] runtime::task<digest> store_remote_blob(blob value);
    [[nodiscard]] runtime::task<void>
    write(std::string path, std::vector<std::byte> contents, write_options options);
    [[nodiscard]] runtime::task<void> redact(std::string path,
                                             std::optional<change_id> change = std::nullopt);
    [[nodiscard]] runtime::task<void> redact(std::string path,
                                             std::vector<std::byte> replacement,
                                             std::optional<change_id> change = std::nullopt);
    [[nodiscard]] runtime::task<std::vector<std::string>>
    diff_keys(std::string path, change_id change_a, change_id change_b);
    [[nodiscard]] runtime::task<void> move(std::string from, std::string to);
    [[nodiscard]] runtime::task<void> remove(std::string path);
    [[nodiscard]] runtime::task<change_id> cut(std::string message);
    [[nodiscard]] runtime::task<change_id> record(change_input input);
    [[nodiscard]] runtime::task<change_id> record_from_tree(change_input input, digest root_tree);
    [[nodiscard]] runtime::task<void> set_metadata(std::string key, std::vector<std::byte> value);
    [[nodiscard]] runtime::task<std::optional<std::vector<std::byte>>> metadata(std::string key);
    [[nodiscard]] runtime::task<std::vector<std::string>> metadata_keys(std::string prefix);
    [[nodiscard]] runtime::task<std::vector<ref>> refs();
    [[nodiscard]] runtime::task<void> set_ref(std::string name, change_id id);
    [[nodiscard]] result<repository_status> status();
    [[nodiscard]] runtime::task<std::vector<change>> log(log_options options = {});
    [[nodiscard]] runtime::task<snapshot> at(change_id id);
    [[nodiscard]] runtime::task<tree> tree(digest id);
    [[nodiscard]] runtime::task<std::vector<std::byte>> blob(digest id);
    [[nodiscard]] runtime::task<readability_report>
    readability(std::optional<change_id> revision = {});
    [[nodiscard]] runtime::task<void> nest(repository source, std::string path);
    [[nodiscard]] runtime::task<void> nest_ref(std::string path, repo_ref reference);
    [[nodiscard]] runtime::task<repository> split(std::string path, std::filesystem::path into);
    [[nodiscard]] runtime::task<void> combine(repository other, std::optional<std::string> at);
    [[nodiscard]] runtime::task<std::vector<conflict>> conflicts();
    [[nodiscard]] runtime::task<std::vector<operation_record>> operations(log_options options = {});
    [[nodiscard]] runtime::task<void> undo(op_id operation);
    [[nodiscard]] runtime::task<void> lock(std::string path);
    [[nodiscard]] runtime::task<void> unlock(std::string path);
    [[nodiscard]] runtime::task<digest> store_attestation(attestation value);
    [[nodiscard]] runtime::task<void> set_policy(policy_config policy);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options = {});
    [[nodiscard]] runtime::task<retention_explanation> retain_explain(digest id);
    [[nodiscard]] runtime::task<cache_report> cache_stats();
    [[nodiscard]] runtime::task<sync_export> export_drips(journal_cursor cursor,
                                                          std::span<sync_item> output);
    [[nodiscard]] runtime::task<sync_import_report> import_drips(std::span<const sync_item> input);
    [[nodiscard]] std::optional<remote_config> remote_configuration() const;

private:
    [[nodiscard]] runtime::task<void>
    redact_impl(std::string path, std::vector<std::byte> replacement, digest target);

    friend struct repository_detail::build_access;
    friend struct repository_detail::testing;
    friend struct repository_detail::vfs_access;
    friend struct repository_detail::git_access;
    friend struct repository_detail::sync_access;
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
