#pragma once

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <map>
#include <optional>
#include <string>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/export.hpp"
#include "liquid/hash.hpp"
#include "liquid/identity.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

struct LIQUID_EXPORT platform {
    std::string os;
    std::string arch;
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
    std::chrono::milliseconds upload{};
};

enum class cache_status : std::uint8_t { miss, hit, divergent };

enum class cache_miss_reason : std::uint8_t {
    input_changed,
    command_changed,
    environment_changed,
    toolchain_changed,
    sandbox_changed,
    never_built,
    divergent_result_quarantined,
    unknown
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
};

struct LIQUID_EXPORT provenance_report {
    result_id result;
    digest attestation;
    liquid::attestation statement;
    action_result value;
    bool verified{};
    std::string reason;
};

struct LIQUID_EXPORT wrap_options {
    std::uint32_t jobs{};
    bool cache{true};
    bool explain{};
    bool verify{true};
    std::optional<std::filesystem::path> cache_directory;
};

struct LIQUID_EXPORT critical_action {
    digest action;
    std::chrono::milliseconds duration{};
    std::vector<std::string> outputs;
};

struct LIQUID_EXPORT wrap_report {
    int exit{};
    std::uint64_t actions{};
    std::uint64_t hits{};
    std::uint64_t misses{};
    std::uint64_t processes_started{};
    cache_status outer{cache_status::miss};
    std::vector<cache_miss> why;
    bool verification_scheduled{};
    cache_report cache;
    std::vector<critical_action> critical_path;
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

struct LIQUID_EXPORT why_report {
    std::string target;
    cache_status lookup{cache_status::miss};
    std::optional<result_id> result;
    std::optional<result_id> previous_result;
    std::optional<digest> action;
    std::optional<digest> previous_action;
    cache_miss cause;
};

namespace build {

[[nodiscard]] LIQUID_EXPORT runtime::task<result_id> execute(repository& source,
                                                             action_input input);
[[nodiscard]] LIQUID_EXPORT runtime::task<action_result> result(repository& source, result_id id);
[[nodiscard]] LIQUID_EXPORT runtime::task<provenance_report> verify_provenance(repository& source,
                                                                               result_id id);
[[nodiscard]] LIQUID_EXPORT runtime::task<provenance_report>
verify_provenance(repository& source, std::string output_path);
[[nodiscard]] LIQUID_EXPORT runtime::task<provenance_report>
verify_wrapped_provenance(std::filesystem::path working_tree, std::filesystem::path output_path);
[[nodiscard]] LIQUID_EXPORT liquid::result<std::string>
provenance_json(const provenance_report& report, bool dsse = false);
[[nodiscard]] LIQUID_EXPORT runtime::task<wrap_report> wrap(std::filesystem::path working_tree,
                                                            std::vector<std::string> command,
                                                            wrap_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<verify_report> verify(repository& source,
                                                                verify_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<verify_report> verify(std::filesystem::path working_tree,
                                                                verify_options options = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<why_report>
why(std::filesystem::path working_tree,
    std::string target,
    std::optional<std::filesystem::path> cache_directory = {});
[[nodiscard]] LIQUID_EXPORT runtime::task<cache_report>
cache_stats(std::filesystem::path working_tree,
            std::optional<std::filesystem::path> cache_directory = {});

} // namespace build

} // namespace liquid
