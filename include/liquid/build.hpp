#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "liquid/abi.hpp"
#include "liquid/attestation.hpp"
#include "liquid/data.hpp"
#include "liquid/export.hpp"
#include "liquid/frontend.hpp"
#include "liquid/graph.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/remote.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;
namespace build_detail {
struct event_access;
}

struct LIQUID_EXPORT platform {
    std::string os;
    std::string arch;
};

struct LIQUID_EXPORT named_input {
    std::string name;
    digest source;
};

struct LIQUID_EXPORT action_input {
    std::vector<std::string> command;
    std::string cwd;
    std::map<std::string, std::string> env;
    std::vector<std::string> outputs;
    digest inputs;
    digest toolchain;
    liquid::platform platform;
    std::optional<std::chrono::seconds> timeout;
    bool remote_eligible{};
    std::vector<named_input> dependency_inputs;
};

struct LIQUID_EXPORT result_id {
    digest value;
};

struct LIQUID_EXPORT action_output {
    std::string path;
    std::uint32_t mode{};
    digest blob;
};

struct LIQUID_EXPORT observed_input {
    std::string path;
    digest content;
};

struct LIQUID_EXPORT action_timing {
    std::chrono::milliseconds queued{};
    std::chrono::milliseconds sandbox{};
    std::chrono::milliseconds exec{};
    std::chrono::milliseconds hash{};
    std::chrono::milliseconds store{};
};

enum class action_size_class : std::uint8_t { unknown, tiny, short_action, medium, long_action };

enum class execution_decision : std::uint8_t { local, remote, race };

struct LIQUID_EXPORT scheduling_inputs {
    std::vector<action_timing> history;
    action_size_class size_class{action_size_class::unknown};
    std::uint64_t input_bytes{};
    bool remote_state_queried{};
    std::uint64_t remote_missing_bytes{};
    std::uint64_t remote_queue_depth{};
    std::uint32_t remote_cache_locality_permille{};
    bool batch_supported{};
    std::uint64_t batch_size{1};
};

struct LIQUID_EXPORT execution_schedule {
    execution_decision decision{execution_decision::local};
    scheduling_inputs inputs;
    std::chrono::milliseconds local_estimate{};
    std::chrono::milliseconds remote_estimate{};
    std::string reason;
};

enum class cache_status : std::uint8_t { miss, hit, divergent };

struct LIQUID_EXPORT build_target {
    std::string name;
};

struct LIQUID_EXPORT build_plan_action {
    std::uint64_t v{1};
    digest action;
    action_input input;
    std::vector<digest> dependencies;
    cache_status state{cache_status::miss};
    std::optional<result_id> result;
};

[[nodiscard]] LIQUID_EXPORT result<std::string> plan_json(const build_plan_action& action);

enum class cache_miss_reason : std::uint8_t {
    input_changed,
    command_changed,
    environment_changed,
    toolchain_changed,
    sandbox_changed,
    never_built,
    divergent_result_quarantined,
    unknown,
    policy_refused,
    non_hermetic_read
};

struct LIQUID_EXPORT line_change {
    std::uint64_t old_line{};
    std::uint64_t old_count{};
    std::uint64_t new_line{};
    std::uint64_t new_count{};
};

struct LIQUID_EXPORT change_attribution {
    digest drip;
    actor by;
    std::chrono::sys_seconds at{};
};

enum class policy_scope : std::uint8_t { paths, toolchain, all_inputs };

enum class policy_on_missing : std::uint8_t { refuse, warn };

struct LIQUID_EXPORT policy_requirement {
    std::string profile;
    std::vector<std::string> from;
    std::uint32_t min_count{1};

    auto operator==(const policy_requirement&) const -> bool = default;
};

struct LIQUID_EXPORT policy_rule {
    policy_scope scope{policy_scope::all_inputs};
    std::vector<std::string> paths;
    std::vector<policy_requirement> require;
    policy_on_missing on_missing{policy_on_missing::refuse};

    auto operator==(const policy_rule&) const -> bool = default;
};

struct LIQUID_EXPORT policy_config {
    std::vector<std::string> approved_builders;
    std::vector<policy_rule> rules;

    auto operator==(const policy_config&) const -> bool = default;
};

struct LIQUID_EXPORT policy_requirement_result {
    policy_requirement requirement;
    std::vector<std::string> identities;
    std::uint32_t count{};
    bool satisfied{};
    std::string reason;
};

struct LIQUID_EXPORT policy_input_result {
    std::string input;
    digest subject;
    bool toolchain{};
    std::uint64_t rule{};
    policy_on_missing on_missing{policy_on_missing::refuse};
    std::vector<policy_requirement_result> requirements;
    bool satisfied{};
};

struct LIQUID_EXPORT policy_report {
    policy_config config;
    digest policy;
    bool allowed{true};
    std::vector<policy_input_result> inputs;
    std::vector<std::string> warnings;
};

struct LIQUID_EXPORT cache_miss {
    cache_miss_reason reason{cache_miss_reason::unknown};
    std::string path;
    std::optional<digest> expected;
    std::optional<digest> actual;
    std::vector<std::string> expected_command;
    std::vector<std::string> actual_command;
    std::string environment_key;
    std::optional<std::string> expected_value;
    std::optional<std::string> actual_value;
    std::string tool;
    std::optional<line_change> lines;
    std::optional<change_attribution> changed_by;
    std::string detail;
};

struct LIQUID_EXPORT cache_tier_report {
    bool available{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t bytes_transferred{};
    std::uint64_t bytes_avoided{};
};

struct LIQUID_EXPORT reproducibility_difference {
    std::string output;
    std::optional<digest> cached;
    std::optional<digest> reexecuted;
    std::optional<std::uint64_t> byte_offset;
    std::optional<std::uint8_t> cached_byte;
    std::optional<std::uint8_t> reexecuted_byte;
};

struct LIQUID_EXPORT critical_cache_alert {
    digest action;
    digest result;
    std::vector<reproducibility_difference> differences;
};

struct LIQUID_EXPORT cache_report {
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t bytes_served{};
    std::uint64_t bytes_stored{};
    std::vector<critical_cache_alert> critical_alerts;
    std::uint64_t bytes_rebuilt{};
    std::map<cache_miss_reason, std::uint64_t> misses_by_reason;
    cache_tier_report memory;
    cache_tier_report disk{.available = true};
    cache_tier_report remote;
};

struct LIQUID_EXPORT action_result {
    digest action;
    int exit{};
    std::vector<action_output> outputs;
    digest stdout_output;
    digest stderr_output;
    std::vector<observed_input> manifest;
    action_timing timing;
    std::string worker;
    std::chrono::sys_seconds observed_at{};
    actor producer;
    std::vector<std::byte> sig;
    cache_status cache{cache_status::miss};
    std::optional<cache_miss> why;
    std::optional<std::chrono::sys_seconds> started_at;
    std::optional<std::chrono::sys_seconds> finished_at;
    std::optional<digest> provenance;
    std::uint64_t record_version{4};
    std::vector<digest> sboms;
    std::vector<digest> abis;
    std::vector<digest> abi_compatibility;
    std::optional<execution_schedule> schedule;
    std::optional<digest> output_tree;
};

struct LIQUID_EXPORT provenance_report {
    result_id result;
    digest attestation;
    liquid::attestation statement;
    action_result value;
    bool verified{};
    std::string reason;
};

enum class sbom_format : std::uint8_t { spdx, cyclonedx };

struct LIQUID_EXPORT sbom_component {
    std::string id;
    std::string name;
    std::string kind;
    std::optional<digest> content;
    std::optional<std::string> repository_id;
    std::optional<digest> pinned_change;
    bool inferred{};
};

struct LIQUID_EXPORT sbom_relationship {
    std::string from;
    std::string to;
    std::string kind;
};

struct LIQUID_EXPORT sbom_report {
    digest subject;
    digest record;
    digest attestation;
    liquid::attestation statement;
    std::string document;
    bool derived{};
    bool inferred{};
    bool verified{};
    std::string reason;
    std::vector<sbom_component> components;
    std::vector<sbom_relationship> relationships;
};

struct LIQUID_EXPORT wrap_options {
    std::uint32_t jobs{};
    bool cache{true};
    bool explain{};
    bool verify{true};
    std::optional<std::string> target;
    std::optional<std::filesystem::path> cache_directory;
    std::optional<std::filesystem::path> fact_directory;
    std::optional<remote_config> remote;
    std::optional<session_id> session;
    std::vector<named_input> inputs;
};

struct LIQUID_EXPORT critical_action {
    result_id result;
    digest action;
    std::chrono::milliseconds duration{};
    action_timing timing;
    cache_status cache{cache_status::miss};
    std::vector<std::string> outputs;
    std::optional<execution_schedule> schedule;
};

enum class build_event_kind : std::uint8_t {
    action_queued,
    action_started,
    action_hit,
    action_done,
    action_failed,
    critical_path_updated,
    test_result,
    build_finished,
};

enum class action_execution_kind : std::uint8_t { local, remote };

struct LIQUID_EXPORT action_execution {
    action_execution_kind kind{action_execution_kind::local};
    std::string worker;
};

enum class test_status : std::uint8_t { passed, failed };

struct LIQUID_EXPORT test_result {
    std::string name;
    test_status status{test_status::passed};
    std::string output;
    std::chrono::milliseconds duration{};
    bool hit{};
};

struct LIQUID_EXPORT test_target {
    std::string name;
    std::vector<std::string> command;
    std::vector<std::string> labels;
    std::vector<std::string> inputs;
    std::string cwd;
    digest toolchain;
    liquid::platform platform;
    std::optional<std::chrono::seconds> timeout;
};

struct LIQUID_EXPORT build_summary {
    int exit{};
    std::uint64_t actions{};
    std::uint64_t hits{};
    std::uint64_t failures{};
    std::chrono::milliseconds duration{};
};

struct LIQUID_EXPORT build_event {
    build_event_kind kind{build_event_kind::action_queued};
    std::uint64_t sequence{};
    std::optional<digest> action;
    std::optional<result_id> result;
    std::optional<action_execution> execution;
    std::vector<action_output> outputs;
    std::vector<frontend_diagnostic> diagnostics;
    std::vector<critical_action> critical_path;
    std::optional<liquid::test_result> test;
    std::optional<build_summary> summary;
};

[[nodiscard]] LIQUID_EXPORT result<std::string> event_json(const build_event& event);

class LIQUID_EXPORT build_event_subscription {
public:
    build_event_subscription() noexcept;
    ~build_event_subscription();
    build_event_subscription(build_event_subscription&&) noexcept;
    auto operator=(build_event_subscription&&) noexcept -> build_event_subscription&;
    build_event_subscription(const build_event_subscription&) = delete;
    auto operator=(const build_event_subscription&) -> build_event_subscription& = delete;

    [[nodiscard]] runtime::task<std::optional<build_event>> next();

private:
    struct impl;
    explicit build_event_subscription(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
    friend class build_handle;
};

class LIQUID_EXPORT build_handle {
public:
    build_handle() noexcept;
    ~build_handle();
    build_handle(build_handle&&) noexcept;
    auto operator=(build_handle&&) noexcept -> build_handle&;
    build_handle(const build_handle&) = delete;
    auto operator=(const build_handle&) -> build_handle& = delete;

    [[nodiscard]] result<build_event_subscription> events() const;
    [[nodiscard]] runtime::task<build_summary> wait();

private:
    struct impl;
    explicit build_handle(std::unique_ptr<impl> implementation) noexcept;
    std::unique_ptr<impl> impl_;
    friend struct build_detail::event_access;
};

struct LIQUID_EXPORT build_trace_action {
    result_id result;
    digest action;
    std::vector<std::string> outputs;
    action_timing timing;
    cache_status cache{cache_status::miss};
    std::chrono::milliseconds start_offset{};
    std::optional<execution_schedule> schedule;
};

struct LIQUID_EXPORT build_trace_edge {
    digest dependency;
    digest dependent;
};

struct LIQUID_EXPORT build_trace {
    digest id;
    std::chrono::milliseconds wall_time{};
    std::vector<build_trace_action> actions;
    std::vector<build_trace_edge> edges;
    cache_report cache;
};

struct LIQUID_EXPORT wrap_report {
    int exit{};
    std::uint64_t actions{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t processes_started{};
    cache_status outer{cache_status::miss};
    std::vector<cache_miss> why;
    std::vector<std::string> policy_warnings;
    bool verification_scheduled{};
    cache_report cache;
    std::optional<digest> trace;
    std::chrono::milliseconds wall_time{};
    std::uint64_t peak_rss_bytes{};
    action_timing phase_totals;
    double parallelism{};
    std::vector<critical_action> critical_path;
    std::vector<build_trace_action> trace_actions;
    std::uint64_t probes_started{};
    std::uint64_t graph_validation_backlog{};
    std::optional<graph_validation_record> last_graph_divergence;
    std::optional<digest> output_tree;
    struct abi_break {
        std::string output;
        digest previous;
        abi_report report;
    };
    std::vector<abi_break> abi_breaks;
};

struct LIQUID_EXPORT known_output {
    digest input_tree;
    digest output_tree;
};

struct LIQUID_EXPORT why_slow_report {
    std::string target;
    digest trace;
    std::chrono::milliseconds duration{};
    std::vector<critical_action> critical_path;
};

struct LIQUID_EXPORT fact_key {
    digest toolchain;
    digest sysroot;
    std::string target;
};

struct LIQUID_EXPORT fact_answer {
    int exit{};
    std::vector<action_output> outputs;
    std::map<std::string, std::string> variables;
    std::optional<digest> result;
};

struct LIQUID_EXPORT configure_fact {
    digest id;
    std::string probe;
    fact_key key;
    fact_answer answer;
};

struct LIQUID_EXPORT fact_bundle {
    digest id;
    std::vector<configure_fact> facts;
};

enum class verify_selection : std::uint8_t { sample, all, changed, action };

struct LIQUID_EXPORT verify_options {
    verify_selection selection{verify_selection::sample};
    std::optional<digest> action;
    bool explain{};
    std::optional<std::filesystem::path> cache_directory;
};

struct LIQUID_EXPORT verified_action {
    digest action;
    digest result;
    digest attestation;
    bool agreement{};
    std::vector<reproducibility_difference> differences;
};

struct LIQUID_EXPORT verify_report {
    std::uint64_t considered{};
    std::vector<verified_action> actions;
};

struct LIQUID_EXPORT graph_verify_options {
    bool all{};
    bool explain{};
    std::uint32_t rate{20};
    std::optional<std::filesystem::path> cache_directory;
};

struct LIQUID_EXPORT graph_verify_report {
    graph_validation_report graph;
    verify_report actions;
};

struct LIQUID_EXPORT replay_options {
    bool record{};
    std::optional<std::filesystem::path> cache_directory;
};

struct LIQUID_EXPORT replay_report {
    digest action;
    digest result;
    int recorded_exit{};
    int reexecuted_exit{};
    bool agreement{};
    std::vector<reproducibility_difference> differences;
    std::optional<digest> attestation;
};

struct LIQUID_EXPORT why_report {
    std::string target;
    cache_status lookup{cache_status::miss};
    std::optional<result_id> result;
    std::optional<result_id> previous_result;
    std::optional<digest> action;
    std::optional<digest> previous_action;
    cache_miss cause;
    std::optional<execution_schedule> schedule;
    cache_report cache;
};

namespace build {

[[nodiscard]] LIQUID_EXPORT result<build_handle> start(repository& source, action_input input);
[[nodiscard]] LIQUID_EXPORT result<build_handle>
start(repository& source, action_input input, remote_executor& remote);
[[nodiscard]] LIQUID_EXPORT result<build_handle> start_wrap(std::filesystem::path working_tree,
                                                            std::vector<std::string> command,
                                                            wrap_options options = {});

[[nodiscard]] LIQUID_EXPORT auto choose_execution(scheduling_inputs inputs) -> execution_schedule;

[[nodiscard]] LIQUID_EXPORT runtime::task<result_id> execute(repository& source,
                                                             action_input input);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<std::byte>> execute_symbol_action(
    repository& source, action_input compile_action, std::vector<std::byte> symbol_action);
[[nodiscard]] LIQUID_EXPORT runtime::task<result_id>
record_remote_result(repository& source, action_input input, action_result value);
[[nodiscard]] LIQUID_EXPORT runtime::task<result_id>
execute(repository& source, action_input input, remote_executor& remote);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<result_id>>
execute_batch(repository& source, std::vector<action_input> inputs, remote_executor& remote);
[[nodiscard]] LIQUID_EXPORT runtime::task<action_result> result(repository& source, result_id id);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<known_output>>
known_outputs(repository& source);
[[nodiscard]] LIQUID_EXPORT runtime::task<provenance_report> verify_provenance(repository& source,
                                                                               result_id id);
[[nodiscard]] LIQUID_EXPORT runtime::task<provenance_report>
verify_provenance(repository& source, std::string output_path);
[[nodiscard]] LIQUID_EXPORT runtime::task<provenance_report>
verify_wrapped_provenance(std::filesystem::path working_tree, std::filesystem::path output_path);
[[nodiscard]] LIQUID_EXPORT liquid::result<std::string>
provenance_json(const provenance_report& report, bool dsse = false);
[[nodiscard]] LIQUID_EXPORT runtime::task<sbom_report>
generate_sbom(repository& source, result_id id, std::string output_path, sbom_format format);
[[nodiscard]] LIQUID_EXPORT runtime::task<sbom_report>
generate_sbom(repository& source, std::string output_path, sbom_format format);
[[nodiscard]] LIQUID_EXPORT runtime::task<sbom_report> generate_wrapped_sbom(
    std::filesystem::path working_tree, std::filesystem::path output_path, sbom_format format);
[[nodiscard]] LIQUID_EXPORT runtime::task<sbom_report> infer_sbom(std::filesystem::path artifact,
                                                                  sbom_format format);
[[nodiscard]] LIQUID_EXPORT runtime::task<wrap_report> wrap(std::filesystem::path working_tree,
                                                            std::vector<std::string> command,
                                                            wrap_options options = {});
[[nodiscard]] LIQUID_EXPORT auto wrap_cache_directory(const std::filesystem::path& working_tree)
    -> liquid::result<std::filesystem::path>;
[[nodiscard]] LIQUID_EXPORT runtime::task<verify_report> verify(repository& source,
                                                                verify_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<verify_report> verify(std::filesystem::path working_tree,
                                                                verify_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<graph_verify_report>
verify_graph(std::filesystem::path working_tree, graph_verify_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<replay_report>
replay(repository& source, digest target, replay_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<replay_report>
replay(std::filesystem::path working_tree, digest target, replay_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<why_report>
why(std::filesystem::path working_tree,
    std::string target,
    std::optional<std::filesystem::path> cache_directory = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<cache_report>
cache_stats(std::filesystem::path working_tree,
            std::optional<std::filesystem::path> cache_directory = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<build_trace>
latest_trace(std::filesystem::path working_tree,
             std::optional<std::filesystem::path> cache_directory = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<why_slow_report>
why_slow(std::filesystem::path working_tree,
         std::string target,
         std::optional<std::filesystem::path> cache_directory = {});
[[nodiscard]] LIQUID_EXPORT liquid::result<std::string> chrome_trace(const build_trace& trace);
[[nodiscard]] LIQUID_EXPORT runtime::task<policy_report> check_policy(repository& source,
                                                                      const action_input& input);
[[nodiscard]] LIQUID_EXPORT runtime::task<policy_report> check_policy(repository& source,
                                                                      result_id target);
[[nodiscard]] LIQUID_EXPORT runtime::task<policy_report>
check_policy(std::filesystem::path working_tree, std::string target);
[[nodiscard]] LIQUID_EXPORT liquid::result<policy_report> explain_policy(repository& source);
[[nodiscard]] LIQUID_EXPORT runtime::task<policy_report>
explain_policy(std::filesystem::path working_tree);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::vector<configure_fact>>
facts_list(std::optional<std::filesystem::path> directory = std::nullopt);
[[nodiscard]] LIQUID_EXPORT runtime::task<configure_fact>
facts_show(digest id, std::optional<std::filesystem::path> directory = std::nullopt);
[[nodiscard]] LIQUID_EXPORT runtime::task<std::string>
facts_export(std::optional<std::filesystem::path> directory = std::nullopt);

} // namespace build

} // namespace liquid
