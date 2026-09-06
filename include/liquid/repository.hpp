#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "liquid/abi.hpp"
#include "liquid/build.hpp"
#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/format.hpp"
#include "liquid/flow.hpp"
#include "liquid/frontend.hpp"
#include "liquid/object.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"

namespace liquid {

namespace repository_detail {
struct build_access;
struct search_access;
struct testing;
struct vfs_access;
struct git_access;
struct drive_access;
struct sync_access;
} // namespace repository_detail

struct LIQUID_EXPORT session_id {
    std::array<std::byte, 16> bytes{};

    auto operator==(const session_id&) const -> bool = default;
};

[[nodiscard]] LIQUID_EXPORT std::string to_string(const session_id& value);
[[nodiscard]] LIQUID_EXPORT result<session_id> parse_session_id(std::string_view text);

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

struct LIQUID_EXPORT agents_config {
    bool require_base{};
    std::vector<std::string> actors;
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
    agents_config agents{};
    policy_config policy{};
    retention_config retention{};
    flow_config flow{};
    std::optional<remote_config> remote;
    std::optional<session_id> session;
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
    std::optional<write_base> base;
    std::vector<std::string> depends_on;
    enum class stale_action : std::uint8_t {
        refuse,
        accept_if_disjoint,
        force,
    } on_stale{stale_action::refuse};
};

struct LIQUID_EXPORT stale_path_change {
    std::string path;
    std::vector<line_change> hunks;
};

struct LIQUID_EXPORT stale_base_report {
    write_base base;
    change_id head;
    std::vector<stale_path_change> changed;
    std::uint64_t since_drips{};
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
    agents_config agents;
};

struct LIQUID_EXPORT snapshot {
    change_id id;
    tree root;
};

enum class tree_diff_kind : std::uint8_t {
    added,
    removed,
    modified,
    moved,
    mode_changed,
    conflicted,
};

enum class container_diff_kind : std::uint8_t {
    added,
    removed,
    modified,
};

struct container_diff;

struct LIQUID_EXPORT container_diff_entry {
    std::string path;
    container_diff_kind kind{container_diff_kind::modified};
    std::optional<digest> old_digest;
    std::optional<digest> new_digest;
    std::shared_ptr<container_diff> nested;
};

struct LIQUID_EXPORT container_diff {
    std::string codec;
    format_tier tier{format_tier::chunk_only};
    std::vector<container_diff_entry> entries;
    bool truncated{};
};

struct LIQUID_EXPORT tree_diff_options {
    bool text{};
    bool formats{};
};

struct LIQUID_EXPORT tree_diff_entry {
    std::string path;
    std::optional<std::string> old_path;
    node_id node{};
    node_kind kind{node_kind::blob};
    std::vector<tree_diff_kind> changes;
    std::optional<std::uint32_t> old_mode;
    std::optional<std::uint32_t> new_mode;
    std::optional<digest> old_blob;
    std::optional<digest> new_blob;
    std::optional<std::vector<line_change>> line_hunks;
    std::optional<container_diff> container;
    std::vector<digest> old_conflicts;
    std::vector<digest> new_conflicts;
};

struct LIQUID_EXPORT text_anchor {
    change_id change;
    std::string path;
    std::uint64_t line{};
    std::uint64_t column{};
};

struct LIQUID_EXPORT mapped_anchor {
    change_id change;
    std::string path;
    std::uint64_t line{};
    std::uint64_t column{};
    bool moved{};
    bool approximate{};
    bool deleted{};
};

struct LIQUID_EXPORT path_history_entry {
    change_id change;
    std::string path;
    node_id node{};
    std::optional<digest> value;
    std::uint32_t mode{0100644};
    actor by;
    std::chrono::sys_seconds at;
    std::string message;
    bool deleted{};
};

struct LIQUID_EXPORT record_cursor {
    std::string token;
};

struct LIQUID_EXPORT record_page_options {
    std::optional<record_cursor> cursor;
    std::uint64_t limit{100};
};

template <class Record> struct record_page {
    std::vector<Record> records;
    std::optional<record_cursor> next;
    bool done{};
};

struct LIQUID_EXPORT repository_result {
    result_id id;
    action_result value;
    std::uint64_t sequence{};
    bool divergent{};
    std::optional<digest> verification;
};

struct LIQUID_EXPORT result_filter {
    std::optional<result_id> id;
    std::optional<digest> action;
    std::optional<std::string> output;
    std::optional<digest> content;
    record_page_options page;
};

struct LIQUID_EXPORT repository_attestation {
    digest id;
    attestation value;
};

struct LIQUID_EXPORT attestation_filter {
    std::optional<digest> id;
    std::optional<digest> subject;
    std::optional<std::string> profile;
    std::optional<std::string> issuer;
    record_page_options page;
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
    session_id session;
};

struct LIQUID_EXPORT undo_skip {
    op_id op;
    std::string path;
    std::string reason;
};

struct LIQUID_EXPORT undo_report {
    std::vector<op_id> applied;
    std::vector<undo_skip> skipped;
};

struct LIQUID_EXPORT undo_options {
    std::vector<std::string> paths;
    std::vector<op_id> drips;
};

struct LIQUID_EXPORT search_options {
    std::optional<std::string> ref;
    std::uint64_t limit{20};
};

enum class search_executor : std::uint8_t {
    bm25f,
    trigram,
    scan,
};

enum class search_query_kind : std::uint8_t {
    ranked_text,
    literal,
    regex_with_literal_core,
    regex_without_literal_core,
};

struct LIQUID_EXPORT search_hit {
    std::string path;
    digest blob{};
    std::uint64_t line{};
    std::uint64_t column{};
    std::string snippet;
    std::optional<double> score;
};

struct LIQUID_EXPORT search_alternative {
    search_executor executor{search_executor::scan};
    std::uint64_t estimated_cost_us{};
    std::optional<std::uint64_t> actual_cost_us;
    bool eligible{};
    std::string reason;
};

struct LIQUID_EXPORT search_plan {
    search_executor executor{search_executor::scan};
    search_query_kind query_kind{search_query_kind::literal};
    std::uint64_t estimated_cost_us{};
    std::uint64_t actual_cost_us{};
    std::uint64_t working_set_blobs{};
    std::uint64_t working_set_bytes{};
    std::uint64_t drips_since_index_update{};
    double trigram_selectivity{};
    std::uint64_t scan_nanoseconds_per_blob{};
    std::uint64_t scan_parallelism{1};
    std::vector<search_alternative> alternatives;
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
    [[nodiscard]] static runtime::task<repository> open_build(std::filesystem::path working_tree,
                                                              repository_options options = {});
    [[nodiscard]] runtime::task<tree> root();
    [[nodiscard]] runtime::task<digest> root_digest();
    [[nodiscard]] runtime::task<change_id> head();
    [[nodiscard]] runtime::task<std::vector<std::byte>> read(std::string path);
    [[nodiscard]] runtime::task<std::vector<std::byte>> read_decrypted(std::string path);
    [[nodiscard]] runtime::task<void> write(std::string path, std::vector<std::byte> contents);
    [[nodiscard]] runtime::task<void>
    write_file(std::string path, std::filesystem::path source, write_options options = {});
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
    [[nodiscard]] runtime::task<void>
    move(std::string from, std::string to, write_options options = {});
    [[nodiscard]] runtime::task<void> remove(std::string path, write_options options = {});
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
    [[nodiscard]] runtime::task<std::vector<drip>> drips();
    [[nodiscard]] runtime::task<stale_base_report> explain_stale(write_base base,
                                                                 std::vector<std::string> paths);
    [[nodiscard]] runtime::task<snapshot> at(change_id id);
    [[nodiscard]] runtime::task<document_state> document_at(change_id id);
    [[nodiscard]] runtime::task<tree> tree(digest id);
    [[nodiscard]] runtime::task<std::vector<std::byte>> blob(digest id);
    [[nodiscard]] runtime::task<std::vector<search_hit>> search(std::string query,
                                                                search_options options = {});
    [[nodiscard]] runtime::task<liquid::search_plan> plan_search(std::string query,
                                                                 search_options options = {});
    [[nodiscard]] runtime::task<std::vector<tree_diff_entry>>
    diff(change_id old_change, change_id new_change, tree_diff_options options = {});
    [[nodiscard]] runtime::task<std::vector<tree_diff_entry>>
    diff(digest old_tree, digest new_tree, tree_diff_options options = {});
    [[nodiscard]] runtime::task<std::vector<tree_diff_entry>>
    diff(liquid::tree old_tree, liquid::tree new_tree, tree_diff_options options = {});
    [[nodiscard]] runtime::task<mapped_anchor> map_anchor(text_anchor anchor, change_id target);
    [[nodiscard]] runtime::task<std::vector<mapped_anchor>>
    map_anchors(std::span<const text_anchor> anchors, change_id target);
    [[nodiscard]] runtime::task<std::vector<path_history_entry>> history(std::string path);
    [[nodiscard]] runtime::task<void> restore(std::string path, change_id change);
    [[nodiscard]] runtime::task<record_page<repository_result>> results(result_filter filter = {});
    [[nodiscard]] runtime::task<action_input> action(digest id);
    [[nodiscard]] runtime::task<record_page<repository_attestation>>
    attestations(attestation_filter filter = {});
    [[nodiscard]] runtime::task<record_page<repository_attestation>>
    attestations(digest subject, record_page_options page = {});
    [[nodiscard]] runtime::task<record_page<build_trace>> traces(record_page_options page = {});
    [[nodiscard]] runtime::task<record_page<sbom_report>> sboms(result_id result,
                                                                record_page_options page = {});
    [[nodiscard]] runtime::task<abi_descriptor> abi(digest library_blob);
    [[nodiscard]] runtime::task<readability_report>
    readability(std::optional<change_id> revision = {});
    [[nodiscard]] runtime::task<void> nest(repository source, std::string path);
    [[nodiscard]] runtime::task<void> nest_ref(std::string path, repo_ref reference);
    [[nodiscard]] runtime::task<repository> split(std::string path, std::filesystem::path into);
    [[nodiscard]] runtime::task<void> combine(repository other, std::optional<std::string> at);
    [[nodiscard]] runtime::task<std::vector<conflict>> conflicts();
    [[nodiscard]] runtime::task<std::vector<operation_record>> operations(log_options options = {});
    [[nodiscard]] runtime::task<undo_report> undo(op_id operation);
    [[nodiscard]] runtime::task<undo_report> undo_session(session_id session,
                                                          undo_options options = {});
    [[nodiscard]] runtime::task<void> lock(std::string path);
    [[nodiscard]] runtime::task<void> unlock(std::string path);
    [[nodiscard]] runtime::task<digest> store_attestation(attestation value);
    [[nodiscard]] runtime::task<digest> store_symbols(symbol_record value);
    [[nodiscard]] runtime::task<symbol_record> symbols(digest id);
    [[nodiscard]] runtime::task<void> set_policy(policy_config policy);
    [[nodiscard]] runtime::task<collection_report> collect(collection_options options = {});
    [[nodiscard]] runtime::task<retention_explanation> retain_explain(digest id);
    [[nodiscard]] runtime::task<cache_report> cache_stats();
    [[nodiscard]] runtime::task<std::vector<build_plan_action>>
    build_plan(build_target target = {});
    [[nodiscard]] result<build_handle> build(build_target target = {});
    [[nodiscard]] result<build_handle> build(action_input input);
    [[nodiscard]] result<build_handle> build(action_input input, remote_executor& remote);
    [[nodiscard]] runtime::task<void> register_test(test_target target);
    [[nodiscard]] runtime::task<std::vector<test_target>> tests();
    [[nodiscard]] result<build_handle> run_tests(std::string filter = {});
    [[nodiscard]] runtime::task<sync_export> export_drips(journal_cursor cursor,
                                                          std::span<sync_item> output);
    [[nodiscard]] runtime::task<sync_import_report> import_drips(std::span<const sync_item> input);
    [[nodiscard]] std::optional<remote_config> remote_configuration() const;
    [[nodiscard]] flow_config flow_configuration() const;
    [[nodiscard]] std::vector<flow_bucket_status> flow_buckets() const;

private:
    [[nodiscard]] runtime::task<std::optional<stale_revalidation>>
    validate_write(write_options options, std::vector<std::string> paths);
    [[nodiscard]] runtime::task<std::vector<digest>> record_ids(std::string kind);
    [[nodiscard]] runtime::task<void>
    redact_impl(std::string path, std::vector<std::byte> replacement, digest target);

    friend struct repository_detail::build_access;
    friend struct repository_detail::search_access;
    friend struct repository_detail::testing;
    friend struct repository_detail::vfs_access;
    friend struct repository_detail::git_access;
    friend struct repository_detail::drive_access;
    friend struct repository_detail::sync_access;
    struct impl;
    std::unique_ptr<impl> impl_;
};

} // namespace liquid
