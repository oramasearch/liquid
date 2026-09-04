#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "build/cache.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "build/why.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"
#include "vfs/repository_access.hpp"

namespace {

class why_fixture {
public:
    why_fixture()
        : root_(
              std::filesystem::temp_directory_path() /
              ("liquid-why-test-" + std::to_string(process_id()) + "-" + std::to_string(next_++))),
          project_(root_ / "project"), cache_(root_ / "cache") {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
        std::filesystem::create_directories(project_, code);
        EXPECT_FALSE(code) << code.message();
        auto opened = liquid::runtime::block_on(liquid::repository::create(
            cache_ / "repository",
            liquid::repository_options{.cache = liquid::cache_config{.publish = true}}));
        EXPECT_TRUE(opened) << opened.error().message;
        repository_ = std::move(*opened);
        auto encoded = liquid::objects::encode(liquid::tree{});
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto stored = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository_, std::move(*encoded)));
        EXPECT_TRUE(stored) << stored.error().message;
        sandbox_ = *stored;
    }

    ~why_fixture() {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
    }

    auto project() const -> const std::filesystem::path& {
        return project_;
    }

    auto cache_path() const -> const std::filesystem::path& {
        return cache_;
    }

    auto repository() -> liquid::repository& {
        return repository_;
    }

    void write(std::string path, std::string contents) {
        auto bytes = std::vector<std::byte>{
            reinterpret_cast<const std::byte*>(contents.data()),
            reinterpret_cast<const std::byte*>(contents.data() + contents.size())};
        auto written =
            liquid::runtime::block_on(repository_.write(std::move(path), std::move(bytes)));
        ASSERT_TRUE(written) << written.error().message;
    }

    auto cut() -> liquid::digest {
        auto cut = liquid::runtime::block_on(repository_.cut("why test input"));
        EXPECT_TRUE(cut) << cut.error().message;
        auto log = liquid::runtime::block_on(repository_.log());
        EXPECT_TRUE(log) << log.error().message;
        return log->front().root;
    }

    auto blob(std::string path) -> liquid::digest {
        auto entry = liquid::runtime::block_on(
            liquid::repository_detail::vfs_access::entry(repository_, std::move(path)));
        EXPECT_TRUE(entry) << entry.error().message;
        return entry->object;
    }

    auto toolchain(std::string path, std::string contents) -> liquid::digest {
        const auto file = root_ / ("tool-" + std::to_string(next_++));
        auto output = std::ofstream{file, std::ios::binary};
        output << contents;
        output.close();
        auto tool = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_file(repository_, file));
        EXPECT_TRUE(tool) << tool.error().message;
        auto name = std::string{"0-"};
        constexpr auto digits = std::string_view{"0123456789abcdef"};
        for (const auto character : path) {
            const auto value = static_cast<unsigned char>(character);
            name.push_back(digits[value >> 4U]);
            name.push_back(digits[value & 0x0fU]);
        }
        auto encoded =
            liquid::objects::encode(liquid::tree{.entries = {{.name = std::move(name),
                                                              .mode = 0755,
                                                              .kind = liquid::node_kind::blob,
                                                              .id = *tool,
                                                              .ref = *tool}}});
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto stored = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository_, std::move(*encoded)));
        EXPECT_TRUE(stored) << stored.error().message;
        return *stored;
    }

    auto publish(std::vector<std::string> command,
                 liquid::digest inputs,
                 liquid::digest toolchain,
                 std::string output,
                 std::string input,
                 liquid::digest content,
                 std::map<std::string, std::string> environment = {{"LANG", "C.UTF-8"}},
                 std::optional<liquid::digest> sandbox = {},
                 std::optional<liquid::execution_schedule> schedule = {}) -> liquid::result_id {
        auto action_input = liquid::action_input{.command = std::move(command),
                                                 .cwd = "",
                                                 .env = std::move(environment),
                                                 .outputs = {output},
                                                 .inputs = inputs,
                                                 .toolchain = toolchain,
                                                 .platform = {"test", "test"}};
        const auto sandbox_id = sandbox.value_or(sandbox_);
        const auto action = liquid::objects::action_record{action_input, sandbox_id};
        auto value = liquid::action_result{.outputs = {{std::move(output), 0644, content}},
                                           .stdout_output = content,
                                           .stderr_output = content,
                                           .manifest = {{std::move(input), content}},
                                           .observed_at = std::chrono::floor<std::chrono::seconds>(
                                               std::chrono::system_clock::now()),
                                           .schedule = std::move(schedule)};
        auto id =
            liquid::runtime::block_on(liquid::build_detail::testing::store_result_with_provenance(
                repository_, action, value));
        EXPECT_TRUE(id) << id.error().message;
        auto cache = liquid::build_detail::disk_cache{repository_};
        EXPECT_TRUE(liquid::runtime::block_on(cache.open()));
        auto candidate = cache.candidate_key(action_input, sandbox_id);
        EXPECT_TRUE(candidate) << candidate.error().message;
        auto published =
            liquid::runtime::block_on(cache.publish(*candidate, liquid::result_id{*id}, value));
        EXPECT_TRUE(published) << published.error().message;
        return {*id};
    }

    void lookup(liquid::result_id current, std::optional<liquid::result_id> previous = {}) {
        auto cache = liquid::build_detail::disk_cache{repository_};
        ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
        auto recorded = cache.record_lookup(liquid::cache_status::miss, current, previous);
        ASSERT_TRUE(recorded) << recorded.error().message;
    }

    void quarantine(liquid::result_id value) {
        auto cache = liquid::build_detail::disk_cache{repository_};
        ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
        auto recorded =
            liquid::runtime::block_on(cache.record_verification(value.value, sandbox_, true));
        ASSERT_TRUE(recorded) << recorded.error().message;
    }

private:
    static auto process_id() -> std::uint64_t {
#if defined(_WIN32)
        return 0;
#else
        return static_cast<std::uint64_t>(::getpid());
#endif
    }

    inline static std::atomic_uint64_t next_{};
    std::filesystem::path root_;
    std::filesystem::path project_;
    std::filesystem::path cache_;
    liquid::repository repository_;
    liquid::digest sandbox_;
};

TEST(BuildWhy, changed_text_input_names_digests_lines_and_drip) {
    why_fixture fixture;
    fixture.write("include/value.h", "#define VALUE 1\n");
    const auto old_root = fixture.cut();
    const auto old_content = fixture.blob("include/value.h");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    const auto old_result = fixture.publish({"cc", "-c", "main.c"},
                                            old_root,
                                            toolchain,
                                            "build/main.o",
                                            "include/value.h",
                                            old_content);

    fixture.write("include/value.h", "#define VALUE 2\n");
    const auto new_root = fixture.cut();
    const auto new_content = fixture.blob("include/value.h");
    const auto new_result = fixture.publish({"cc", "-c", "main.c"},
                                            new_root,
                                            toolchain,
                                            "build/main.o",
                                            "include/value.h",
                                            new_content);
    fixture.lookup(new_result, old_result);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "build/main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::input_changed);
    EXPECT_EQ(report->cause.path, "include/value.h");
    ASSERT_TRUE(report->cause.expected);
    ASSERT_TRUE(report->cause.actual);
    EXPECT_EQ(liquid::to_string(*report->cause.expected), liquid::to_string(old_content));
    EXPECT_EQ(liquid::to_string(*report->cause.actual), liquid::to_string(new_content));
    ASSERT_TRUE(report->cause.lines);
    EXPECT_EQ(report->cause.lines->old_line, 1U);
    EXPECT_EQ(report->cause.lines->new_line, 1U);
    ASSERT_TRUE(report->cause.changed_by);
    EXPECT_FALSE(report->cause.changed_by->by.taap_id.empty());
}

TEST(BuildWhy, changed_command_reports_the_argv_diff) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    const auto old_result =
        fixture.publish({"cc", "-O0", "main.c"}, root, toolchain, "main.o", "main.c", content);
    const auto new_result =
        fixture.publish({"cc", "-O2", "main.c"}, root, toolchain, "main.o", "main.c", content);
    fixture.lookup(new_result, old_result);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::command_changed);
    EXPECT_EQ(report->cause.expected_command[1], "-O0");
    EXPECT_EQ(report->cause.actual_command[1], "-O2");

    auto by_result = liquid::runtime::block_on(liquid::build::why(
        fixture.project(), liquid::to_string(new_result.value), fixture.cache_path()));
    ASSERT_TRUE(by_result) << by_result.error().message;
    EXPECT_EQ(by_result->cause.reason, liquid::cache_miss_reason::command_changed);
    ASSERT_TRUE(by_result->action);
    auto by_action = liquid::runtime::block_on(liquid::build::why(
        fixture.project(), liquid::to_string(*by_result->action), fixture.cache_path()));
    ASSERT_TRUE(by_action) << by_action.error().message;
    EXPECT_EQ(by_action->cause.reason, liquid::cache_miss_reason::command_changed);
}

TEST(BuildWhy, reports_execution_decision_and_every_model_input) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    const auto schedule =
        liquid::execution_schedule{.decision = liquid::execution_decision::remote,
                                   .inputs = {.history = {{.exec = std::chrono::milliseconds{800}}},
                                              .size_class = liquid::action_size_class::medium,
                                              .input_bytes = 8192,
                                              .remote_state_queried = true,
                                              .remote_missing_bytes = 2048,
                                              .remote_queue_depth = 3,
                                              .remote_cache_locality_permille = 750,
                                              .batch_supported = true,
                                              .batch_size = 4},
                                   .local_estimate = std::chrono::milliseconds{800},
                                   .remote_estimate = std::chrono::milliseconds{720},
                                   .reason = "remote estimate is lower"};
    const auto value = fixture.publish(
        {"cc", "main.c"}, root, toolchain, "main.o", "main.c", content, {}, {}, schedule);
    fixture.lookup(value);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_TRUE(report->schedule);
    EXPECT_EQ(report->schedule->decision, liquid::execution_decision::remote);
    EXPECT_EQ(report->schedule->inputs.history.size(), 1U);
    EXPECT_EQ(report->schedule->inputs.size_class, liquid::action_size_class::medium);
    EXPECT_EQ(report->schedule->inputs.input_bytes, 8192U);
    EXPECT_TRUE(report->schedule->inputs.remote_state_queried);
    EXPECT_EQ(report->schedule->inputs.remote_missing_bytes, 2048U);
    EXPECT_EQ(report->schedule->inputs.remote_queue_depth, 3U);
    EXPECT_EQ(report->schedule->inputs.remote_cache_locality_permille, 750U);
    EXPECT_TRUE(report->schedule->inputs.batch_supported);
    EXPECT_EQ(report->schedule->inputs.batch_size, 4U);
}

TEST(BuildWhy, imported_action_index_explains_without_a_local_lookup_journal) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    fixture.publish({"cc", "-O0", "main.c"}, root, toolchain, "main.o", "main.c", content);
    fixture.publish({"cc", "-O2", "main.c"}, root, toolchain, "main.o", "main.c", content);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::command_changed);
    EXPECT_EQ(report->cause.expected_command[1], "-O0");
    EXPECT_EQ(report->cause.actual_command[1], "-O2");
}

TEST(BuildWhy, changed_toolchain_names_the_tool_and_tree_digests) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto old_toolchain = fixture.toolchain("/usr/bin/cc", "compiler one");
    const auto new_toolchain = fixture.toolchain("/usr/bin/cc", "compiler two");
    const auto old_result =
        fixture.publish({"cc", "main.c"}, root, old_toolchain, "main.o", "main.c", content);
    const auto new_result =
        fixture.publish({"cc", "main.c"}, root, new_toolchain, "main.o", "main.c", content);
    fixture.lookup(new_result, old_result);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::toolchain_changed);
    EXPECT_EQ(report->cause.tool, "/usr/bin/cc");
    ASSERT_TRUE(report->cause.expected);
    ASSERT_TRUE(report->cause.actual);
    EXPECT_EQ(liquid::to_string(*report->cause.expected), liquid::to_string(old_toolchain));
    EXPECT_EQ(liquid::to_string(*report->cause.actual), liquid::to_string(new_toolchain));
}

TEST(BuildWhy, changed_environment_names_the_key) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    const auto old_result = fixture.publish(
        {"cc", "main.c"}, root, toolchain, "main.o", "main.c", content, {{"MODE", "debug"}});
    const auto new_result = fixture.publish(
        {"cc", "main.c"}, root, toolchain, "main.o", "main.c", content, {{"MODE", "release"}});
    fixture.lookup(new_result, old_result);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::environment_changed);
    EXPECT_EQ(report->cause.environment_key, "MODE");
    EXPECT_EQ(report->cause.expected_value, "debug");
    EXPECT_EQ(report->cause.actual_value, "release");
}

TEST(BuildWhy, changed_sandbox_names_both_definition_digests) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    const auto old_sandbox = fixture.toolchain("sandbox", "one");
    const auto new_sandbox = fixture.toolchain("sandbox", "two");
    const auto old_result = fixture.publish(
        {"cc", "main.c"}, root, toolchain, "main.o", "main.c", content, {}, old_sandbox);
    const auto new_result = fixture.publish(
        {"cc", "main.c"}, root, toolchain, "main.o", "main.c", content, {}, new_sandbox);
    fixture.lookup(new_result, old_result);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::sandbox_changed);
    ASSERT_TRUE(report->cause.expected);
    ASSERT_TRUE(report->cause.actual);
    EXPECT_EQ(liquid::to_string(*report->cause.expected), liquid::to_string(old_sandbox));
    EXPECT_EQ(liquid::to_string(*report->cause.actual), liquid::to_string(new_sandbox));
}

TEST(BuildWhy, target_without_a_record_is_never_built) {
    why_fixture fixture;
    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "missing.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::never_built);
}

TEST(BuildWhy, quarantined_result_uses_the_authoritative_index_flag) {
    why_fixture fixture;
    fixture.write("main.c", "int main(void) { return 0; }\n");
    const auto root = fixture.cut();
    const auto content = fixture.blob("main.c");
    const auto toolchain = fixture.toolchain("/usr/bin/cc", "compiler");
    const auto result =
        fixture.publish({"cc", "main.c"}, root, toolchain, "main.o", "main.c", content);
    fixture.quarantine(result);
    fixture.lookup(result);

    auto report = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "main.o", fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->cause.reason, liquid::cache_miss_reason::divergent_result_quarantined);
    EXPECT_NE(report->cause.detail.find("verification attestation"), std::string::npos);
}

TEST(BuildWhy, default_cache_identity_survives_build_configuration_changes) {
    why_fixture fixture;
    fixture.write("CMakeLists.txt", "set(FLAGS -O0)\n");
    const auto before = liquid::build_detail::wrap_cache_directory(fixture.project());
    ASSERT_TRUE(before) << before.error().message;
    fixture.write("CMakeLists.txt", "set(FLAGS -O2)\n");
    const auto after = liquid::build_detail::wrap_cache_directory(fixture.project());
    ASSERT_TRUE(after) << after.error().message;
    EXPECT_EQ(*before, *after);
}

TEST(BuildWhy, cumulative_stats_match_wrap_action_counts_and_bytes) {
    why_fixture fixture;
    auto first = liquid::wrap_report{.actions = 3, .hits = 0, .misses = 3};
    first.cache.bytes_rebuilt = 120;
    first.cache.misses_by_reason[liquid::cache_miss_reason::never_built] = 3;
    ASSERT_TRUE(liquid::build_detail::update_cache_stats(fixture.cache_path(), first));
    auto second = liquid::wrap_report{.actions = 3, .hits = 3, .misses = 0};
    second.cache.bytes_served = 12;
    second.cache.disk.bytes_avoided = 120;
    ASSERT_TRUE(liquid::build_detail::update_cache_stats(fixture.cache_path(), second));

    auto report = liquid::runtime::block_on(
        liquid::build::cache_stats(fixture.project(), fixture.cache_path()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->hits, 3U);
    EXPECT_EQ(report->misses, 3U);
    EXPECT_EQ(report->disk.hits + report->disk.misses, 6U);
    EXPECT_EQ(report->disk.bytes_transferred, 12U);
    EXPECT_EQ(report->disk.bytes_avoided, 120U);
    EXPECT_EQ(report->bytes_rebuilt, 120U);
}

} // namespace
