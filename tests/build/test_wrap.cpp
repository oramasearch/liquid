#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <string>
#include <vector>

#if !defined(_WIN32)
#include <unistd.h>
#endif

#include "build/cache.hpp"
#include "build/repository_access.hpp"
#include "formats/json.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"

namespace {

class wrap_directory {
public:
    explicit wrap_directory(std::string_view fixture)
        : root_(
              std::filesystem::temp_directory_path() /
              ("liquid-wrap-test-" + std::to_string(process_id()) + "-" + std::to_string(next_++))),
          project_(root_ / "project"), cache_(root_ / "cache") {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
        std::filesystem::create_directories(root_, code);
        EXPECT_FALSE(code) << code.message();
        std::filesystem::copy(std::filesystem::path{LIQUID_WRAP_FIXTURES} / fixture,
                              project_,
                              std::filesystem::copy_options::recursive,
                              code);
        EXPECT_FALSE(code) << code.message();
    }

    ~wrap_directory() {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
    }

    auto project() const -> const std::filesystem::path& {
        return project_;
    }

    auto cache() const -> const std::filesystem::path& {
        return cache_;
    }

    auto second_checkout(std::string_view fixture = "cmake") const -> std::filesystem::path {
        const auto path = root_ / "second-project";
        auto code = std::error_code{};
        std::filesystem::copy(std::filesystem::path{LIQUID_WRAP_FIXTURES} / fixture,
                              path,
                              std::filesystem::copy_options::recursive,
                              code);
        EXPECT_FALSE(code) << code.message();
        return path;
    }

    auto second_cache() const -> std::filesystem::path {
        return root_ / "second-cache";
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
};

auto available(std::string_view command) -> bool {
    if (command == "cmake")
        return std::system("command -v cmake >/dev/null 2>&1") == 0;
    if (command == "ninja")
        return std::system("command -v ninja >/dev/null 2>&1") == 0;
    if (command == "make")
        return std::system("command -v make >/dev/null 2>&1") == 0;
    if (command == "clang")
        return std::system("command -v clang >/dev/null 2>&1") == 0 ||
               std::system("command -v clang-19 >/dev/null 2>&1") == 0;
    if (command == "autoconf")
        return std::system("command -v autoconf >/dev/null 2>&1") == 0;
    return false;
}

auto run_wrap(const std::filesystem::path& project,
              const std::filesystem::path& cache,
              std::vector<std::string> command,
              bool explain = false,
              std::uint32_t jobs = 0,
              std::optional<liquid::remote_config> remote = {})
    -> liquid::result<liquid::wrap_report> {
    return liquid::runtime::block_on(
        liquid::build::wrap(project,
                            std::move(command),
                            liquid::wrap_options{.jobs = jobs,
                                                 .cache = true,
                                                 .explain = explain,
                                                 .verify = false,
                                                 .cache_directory = cache,
                                                 .fact_directory = cache.parent_path() / "facts",
                                                 .remote = std::move(remote)}));
}

auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output += character;
        }
    }
    output += '\'';
    return output;
}

auto tree_blobs(liquid::repository& repository, liquid::digest root)
    -> liquid::result<std::vector<liquid::digest>> {
    auto bytes =
        liquid::runtime::block_on(liquid::repository_detail::build_access::get(repository, root));
    if (!bytes) {
        return std::unexpected(bytes.error());
    }
    auto tree = liquid::objects::decode_tree(*bytes);
    if (!tree) {
        return std::unexpected(tree.error());
    }
    auto output = std::vector<liquid::digest>{};
    for (const auto& entry : tree->entries) {
        if (entry.kind == liquid::node_kind::blob) {
            output.push_back(entry.ref);
            continue;
        }
        if (entry.kind == liquid::node_kind::tree) {
            auto nested = tree_blobs(repository, entry.ref);
            if (!nested) {
                return std::unexpected(nested.error());
            }
            output.insert(output.end(), nested->begin(), nested->end());
        }
    }
    return output;
}

#if LIQUID_BUILD_HAS_FUSE && defined(__linux__)

class scoped_environment {
public:
    scoped_environment(std::string name, std::string value) : name_(std::move(name)) {
        if (const auto* previous = std::getenv(name_.c_str())) {
            previous_ = previous;
        }
        valid_ = ::setenv(name_.c_str(), value.c_str(), 1) == 0;
    }

    scoped_environment(const scoped_environment&) = delete;
    auto operator=(const scoped_environment&) -> scoped_environment& = delete;

    ~scoped_environment() {
        if (previous_) {
            static_cast<void>(::setenv(name_.c_str(), previous_->c_str(), 1));
        } else {
            static_cast<void>(::unsetenv(name_.c_str()));
        }
    }

    [[nodiscard]] bool valid() const noexcept {
        return valid_;
    }

private:
    std::string name_;
    std::optional<std::string> previous_;
    bool valid_{};
};

TEST(BuildWrap, cmake_caches_children_outer_action_and_fixed_root_across_checkouts) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("cmake") || !available("ninja") || !available("clang"))
        GTEST_SKIP() << "cmake, ninja, or clang is absent";
    wrap_directory fixture{"cmake"};
    auto configured = run_wrap(
        fixture.project(), fixture.cache(), {"cmake", "-S", ".", "-B", "build", "-G", "Ninja"});
    ASSERT_TRUE(configured) << configured.error().message << ": " << configured.error().context;
    ASSERT_EQ(configured->exit, 0);
    auto first =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, false, 3);
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->exit, 0);
    EXPECT_GE(first->processes_started, 3U);
    EXPECT_TRUE(std::filesystem::is_regular_file(fixture.project() / "build/wrapped"));

    auto repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(repository) << repository.error().message;
    auto cache = liquid::build_detail::disk_cache{*repository};
    auto cache_opened = liquid::runtime::block_on(cache.open());
    ASSERT_TRUE(cache_opened) << cache_opened.error().message;
    auto producing = liquid::runtime::block_on(cache.find_output("build/wrapped"));
    ASSERT_TRUE(producing) << producing.error().message;
    ASSERT_TRUE(*producing);
    auto result_bytes = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(*repository, (**producing).value));
    ASSERT_TRUE(result_bytes) << result_bytes.error().message;
    auto result = liquid::objects::decode_result(*result_bytes);
    ASSERT_TRUE(result) << result.error().message;
    auto action_bytes = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(*repository, result->action));
    ASSERT_TRUE(action_bytes) << action_bytes.error().message;
    auto action = liquid::objects::decode_action(*action_bytes);
    ASSERT_TRUE(action) << action.error().message;
    auto sbom = liquid::runtime::block_on(liquid::build::generate_sbom(
        *repository, **producing, "build/wrapped", liquid::sbom_format::spdx));
    ASSERT_TRUE(sbom) << sbom.error().message;
    ASSERT_TRUE(sbom->verified) << sbom->reason;
    for (const auto& input : result->manifest) {
        EXPECT_TRUE(std::ranges::any_of(sbom->components, [&](const auto& component) {
            return component.content &&
                   liquid::to_string(*component.content) == liquid::to_string(input.content);
        })) << input.path;
    }
    auto tools = tree_blobs(*repository, action->input.toolchain);
    ASSERT_TRUE(tools) << tools.error().message;
    ASSERT_FALSE(tools->empty());
    for (const auto& tool : *tools) {
        EXPECT_TRUE(std::ranges::any_of(sbom->components, [&](const auto& component) {
            return component.kind == "tool" && component.content &&
                   liquid::to_string(*component.content) == liquid::to_string(tool);
        })) << liquid::to_string(tool);
    }

    const auto before_second = repository->status();
    ASSERT_TRUE(before_second) << before_second.error().message;
    auto second =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, false, 3);
    ASSERT_TRUE(second) << second.error().message << ": " << second.error().context;
    EXPECT_EQ(second->exit, 0);
    EXPECT_EQ(second->processes_started, 0U);
    EXPECT_EQ(second->outer, liquid::cache_status::hit);
    EXPECT_EQ(second->hits + second->misses, second->actions);
    EXPECT_LT(second->peak_rss_bytes, std::uint64_t{512 * 1024 * 1024});
    auto after_second_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(after_second_repository) << after_second_repository.error().message;
    const auto after_second = after_second_repository->status();
    ASSERT_TRUE(after_second) << after_second.error().message;
    EXPECT_EQ(after_second->pending_drips, before_second->pending_drips);
    auto statistics =
        liquid::runtime::block_on(liquid::build::cache_stats(fixture.project(), fixture.cache()));
    ASSERT_TRUE(statistics) << statistics.error().message;
    EXPECT_EQ(statistics->hits, configured->hits + first->hits + second->hits);
    EXPECT_EQ(statistics->misses, configured->misses + first->misses + second->misses);
    EXPECT_GT(statistics->disk.bytes_avoided, 0U);

    auto left_before =
        liquid::runtime::block_on(cache.find_output("build/CMakeFiles/wrapped.dir/left.c.o"));
    ASSERT_TRUE(left_before);
    ASSERT_TRUE(*left_before);
    auto left_before_bytes = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(*repository, (**left_before).value));
    ASSERT_TRUE(left_before_bytes);
    auto left_before_result = liquid::objects::decode_result(*left_before_bytes);
    ASSERT_TRUE(left_before_result);
    const auto left_before_output = std::ranges::find(left_before_result->outputs,
                                                      "build/CMakeFiles/wrapped.dir/left.c.o",
                                                      &liquid::action_output::path);
    ASSERT_NE(left_before_output, left_before_result->outputs.end());
    const auto left_object_before = left_before_output->blob;
    const auto link_before = (**producing).value;

    auto source = std::ofstream{fixture.project() / "left.c", std::ios::app};
    source << "\n// fingerprint-preserving edit\n";
    source.close();
    auto comment =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, true, 3);
    ASSERT_TRUE(comment) << comment.error().message << ": " << comment.error().context;
    EXPECT_EQ(comment->processes_started, 1U);

    auto comment_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(comment_repository);
    auto comment_cache = liquid::build_detail::disk_cache{*comment_repository};
    ASSERT_TRUE(liquid::runtime::block_on(comment_cache.open()));
    auto left_after = liquid::runtime::block_on(
        comment_cache.find_output("build/CMakeFiles/wrapped.dir/left.c.o"));
    auto link_after = liquid::runtime::block_on(comment_cache.find_output("build/wrapped"));
    ASSERT_TRUE(left_after);
    ASSERT_TRUE(*left_after);
    ASSERT_TRUE(link_after);
    ASSERT_TRUE(*link_after);
    auto left_after_bytes = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(*comment_repository, (**left_after).value));
    ASSERT_TRUE(left_after_bytes);
    auto left_after_result = liquid::objects::decode_result(*left_after_bytes);
    ASSERT_TRUE(left_after_result);
    const auto left_after_output = std::ranges::find(left_after_result->outputs,
                                                     "build/CMakeFiles/wrapped.dir/left.c.o",
                                                     &liquid::action_output::path);
    ASSERT_NE(left_after_output, left_after_result->outputs.end());
    EXPECT_EQ(liquid::to_string(left_after_output->blob), liquid::to_string(left_object_before));
    EXPECT_EQ(liquid::to_string((**link_after).value), liquid::to_string(link_before));

    auto left_source = std::ifstream{fixture.project() / "left.c", std::ios::binary};
    auto left_text =
        std::string{std::istreambuf_iterator<char>{left_source}, std::istreambuf_iterator<char>{}};
    const auto return_value = left_text.find("return WRAPPED_VALUE;");
    ASSERT_NE(return_value, std::string::npos);
    left_text.replace(return_value,
                      std::string_view{"return WRAPPED_VALUE;"}.size(),
                      "return WRAPPED_VALUE + 1;");
    auto changed_source = std::ofstream{fixture.project() / "left.c", std::ios::trunc};
    changed_source << left_text;
    changed_source.close();
    auto semantic =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, true, 3);
    ASSERT_TRUE(semantic) << semantic.error().message << ": " << semantic.error().context;
    EXPECT_EQ(semantic->processes_started, 2U);

    auto semantic_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(semantic_repository);
    auto semantic_cache = liquid::build_detail::disk_cache{*semantic_repository};
    ASSERT_TRUE(liquid::runtime::block_on(semantic_cache.open()));
    auto semantic_link = liquid::runtime::block_on(semantic_cache.find_output("build/wrapped"));
    ASSERT_TRUE(semantic_link);
    ASSERT_TRUE(*semantic_link);
    EXPECT_NE(liquid::to_string((**semantic_link).value), liquid::to_string(link_before));

    auto header = std::ofstream{fixture.project() / "include/value.h", std::ios::app};
    header << "\n#define WRAPPED_SECOND_VALUE 1\n";
    header.close();
    auto changed =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, true, 3);
    ASSERT_TRUE(changed) << changed.error().message << ": " << changed.error().context;
    EXPECT_EQ(changed->exit, 0);
    EXPECT_GT(changed->processes_started, 0U);
    EXPECT_LT(changed->processes_started, first->processes_started);
    EXPECT_TRUE(std::ranges::any_of(
        changed->why, [](const auto& miss) { return miss.path == "include/value.h"; }));
    auto explanation = liquid::runtime::block_on(liquid::build::why(
        fixture.project(), "build/CMakeFiles/wrapped.dir/left.c.o", fixture.cache()));
    ASSERT_TRUE(explanation) << explanation.error().message;
    ASSERT_TRUE(explanation->result);
    const auto explanation_context =
        liquid::to_string(explanation->result->value) + " after " +
        (explanation->previous_result ? liquid::to_string(explanation->previous_result->value)
                                      : std::string{"none"});
    EXPECT_EQ(explanation->cause.reason, liquid::cache_miss_reason::input_changed)
        << explanation->cause.detail << ": " << explanation_context;
    EXPECT_EQ(explanation->cause.path, "include/value.h")
        << explanation->cause.detail << ": " << explanation_context;
    EXPECT_TRUE(explanation->cause.expected);
    EXPECT_TRUE(explanation->cause.actual);
    EXPECT_TRUE(explanation->cause.changed_by);

    const auto other = fixture.second_checkout();
    auto other_configured =
        run_wrap(other, fixture.cache(), {"cmake", "-S", ".", "-B", "build", "-G", "Ninja"});
    ASSERT_TRUE(other_configured) << other_configured.error().message;
    auto other_built = run_wrap(other, fixture.cache(), {"cmake", "--build", "build"}, false, 3);
    ASSERT_TRUE(other_built) << other_built.error().message;
    EXPECT_EQ(other_built->processes_started, 0U);
    EXPECT_EQ(other_built->outer, liquid::cache_status::hit);
    EXPECT_EQ(std::getenv("LD_PRELOAD"), nullptr);
    EXPECT_EQ(std::getenv("DYLD_INSERT_LIBRARIES"), nullptr);
}

TEST(BuildWrap, recursive_make_is_split_into_actions) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("make") || !available("clang"))
        GTEST_SKIP() << "make or clang is absent";
    wrap_directory make_fixture{"make"};
    auto made =
        run_wrap(make_fixture.project(), make_fixture.cache(), {"make", "-C", "sub"}, false, 3);
    ASSERT_TRUE(made) << made.error().message << ": " << made.error().context;
    ASSERT_EQ(made->exit, 0);
    EXPECT_GE(made->processes_started, 3U);
    auto made_again =
        run_wrap(make_fixture.project(), make_fixture.cache(), {"make", "-C", "sub"}, false, 3);
    ASSERT_TRUE(made_again) << made_again.error().message;
    EXPECT_EQ(made_again->outer, liquid::cache_status::hit);
}

TEST(BuildWrap, reapi_action_cache_matches_the_local_second_run_hit_rate) {
    const auto* endpoint = std::getenv("LIQUID_REAPI_ENDPOINT");
    if (!endpoint) {
        GTEST_SKIP() << "LIQUID_REAPI_ENDPOINT is not configured";
    }
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    if (!available("make") || !available("clang")) {
        GTEST_SKIP() << "make or clang is absent";
    }
    wrap_directory fixture{"make"};
    auto remote = liquid::remote_config{.endpoint = endpoint, .publish = true};
    auto first = run_wrap(fixture.project(), fixture.cache(), {"make"}, false, 3, remote);
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->exit, 0);
    ASSERT_GT(first->misses, 0U);

    const auto actions = fixture.cache() / "repository/.liquid-store/actions";
    auto code = std::error_code{};
    EXPECT_TRUE(std::filesystem::remove(actions / "INDEX", code));
    ASSERT_FALSE(code) << code.message();
    EXPECT_TRUE(std::filesystem::remove(actions / "ROOT", code));
    ASSERT_FALSE(code) << code.message();

    remote.publish = false;
    auto second = run_wrap(fixture.project(), fixture.cache(), {"make"}, false, 3, remote);
    ASSERT_TRUE(second) << second.error().message << ": " << second.error().context;
    EXPECT_EQ(second->exit, 0);
    EXPECT_EQ(second->outer, liquid::cache_status::hit);
    EXPECT_EQ(second->hits, second->actions);
    EXPECT_EQ(second->misses, 0U);
    EXPECT_EQ(second->cache.remote.hits, 1U);
    EXPECT_EQ(second->cache.remote.misses, 0U);
}

TEST(BuildWrap, trace_records_longest_dependent_chain_and_exports_chrome_json) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    wrap_directory fixture{"make"};
    const auto tool = fixture.project() / "sleep-tool";
    auto code = std::error_code{};
    std::filesystem::copy_file(
        LIQUID_WRAP_SLEEP_TOOL, tool, std::filesystem::copy_options::overwrite_existing, code);
    ASSERT_FALSE(code) << code.message();
    std::filesystem::permissions(tool,
                                 std::filesystem::perms::owner_exec |
                                     std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write,
                                 std::filesystem::perm_options::replace,
                                 code);
    ASSERT_FALSE(code) << code.message();
    {
        auto input = std::ofstream{fixture.project() / "trace-input.txt"};
        input << "critical path\n";
    }
    const auto script = "./sleep-tool trace-input.txt trace-middle.txt 80 && "
                        "./sleep-tool trace-middle.txt trace-final.txt 30";
    auto cache_environment = scoped_environment{"LIQUID_WRAP_CACHE", fixture.cache().string()};
    ASSERT_TRUE(cache_environment.valid());
    auto wrapped = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(),
                            {"/bin/sh", "-c", script},
                            liquid::wrap_options{.cache = true, .verify = false}));
    ASSERT_TRUE(wrapped) << wrapped.error().message << ": " << wrapped.error().context;
    ASSERT_EQ(wrapped->exit, 0);
    ASSERT_TRUE(wrapped->trace);
    auto trace = liquid::runtime::block_on(liquid::build::latest_trace(fixture.project()));
    ASSERT_TRUE(trace) << trace.error().message;
    EXPECT_EQ(liquid::to_string(trace->id), liquid::to_string(*wrapped->trace));
    ASSERT_EQ(trace->actions.size(), 2U);
    EXPECT_GE(trace->edges.size(), 1U);
    auto explanation =
        liquid::runtime::block_on(liquid::build::why_slow(fixture.project(), "trace-final.txt"));
    ASSERT_TRUE(explanation) << explanation.error().message;
    ASSERT_EQ(explanation->critical_path.size(), 2U);
    EXPECT_GE(explanation->duration.count(), 100);
    EXPECT_TRUE(
        std::ranges::any_of(explanation->critical_path.front().outputs,
                            [](const auto& output) { return output == "trace-middle.txt"; }));
    EXPECT_TRUE(
        std::ranges::any_of(explanation->critical_path.back().outputs,
                            [](const auto& output) { return output == "trace-final.txt"; }));
    for (const auto& action : trace->actions) {
        ASSERT_TRUE(action.schedule);
        EXPECT_EQ(action.schedule->decision, liquid::execution_decision::local);
        const auto phases = action.timing.queued + action.timing.sandbox + action.timing.exec +
                            action.timing.hash + action.timing.store;
        const auto found = std::ranges::find_if(explanation->critical_path, [&](const auto& item) {
            return liquid::to_string(item.action) == liquid::to_string(action.action);
        });
        if (found != explanation->critical_path.end()) {
            EXPECT_EQ(found->duration, phases);
        }
        if (std::ranges::find(action.outputs, "trace-middle.txt") != action.outputs.end()) {
            EXPECT_GE(action.timing.exec.count(), 70);
        }
        if (std::ranges::find(action.outputs, "trace-final.txt") != action.outputs.end()) {
            EXPECT_GE(action.timing.exec.count(), 20);
        }
    }
    auto chrome = liquid::build::chrome_trace(*trace);
    ASSERT_TRUE(chrome) << chrome.error().message;
    for (const auto field : {"execution_decision",
                             "history_ms",
                             "size_class",
                             "input_bytes",
                             "remote_state_queried",
                             "remote_missing_bytes",
                             "remote_queue_depth",
                             "remote_cache_locality_permille",
                             "batch_supported",
                             "batch_size",
                             "local_estimate_ms",
                             "remote_estimate_ms",
                             "decision_reason"}) {
        EXPECT_NE(chrome->find(field), std::string::npos) << field;
    }
    auto json = liquid::formats::json::parse(*chrome);
    ASSERT_TRUE(json) << json.error().message;
    const auto* object = std::get_if<liquid::formats::json::value::object>(&json->data);
    ASSERT_NE(object, nullptr);
    EXPECT_NE(std::ranges::find(*object,
                                "traceEvents",
                                [](const auto& item) { return std::string_view{item.first}; }),
              object->end());
#if defined(LIQUID_CLI)
    const auto output = fixture.project().parent_path() / "why-slow-output";
    const auto command = "cd " + shell_quote(fixture.project().string()) + " && " +
                         shell_quote(LIQUID_CLI) + " why-slow trace-final.txt >" +
                         shell_quote(output.string()) + " 2>&1";
    const auto status = std::system(command.c_str());
    auto input = std::ifstream{output};
    const auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(status, 0) << text;
    EXPECT_NE(text.find("output=trace-middle.txt"), std::string::npos) << text;
    EXPECT_NE(text.find("output=trace-final.txt"), std::string::npos) << text;
    EXPECT_NE(text.find("decision=local"), std::string::npos) << text;
    EXPECT_NE(text.find("history_ms="), std::string::npos) << text;

    const auto why_output = fixture.project().parent_path() / "why-schedule-output";
    const auto why_command = "cd " + shell_quote(fixture.project().string()) + " && " +
                             shell_quote(LIQUID_CLI) + " why trace-final.txt --json >" +
                             shell_quote(why_output.string()) + " 2>&1";
    const auto why_status = std::system(why_command.c_str());
    auto why_input = std::ifstream{why_output};
    const auto why_text =
        std::string{std::istreambuf_iterator<char>{why_input}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(why_status, 0) << why_text;
    EXPECT_NE(why_text.find("\"schedule\":{\"decision\":\"local\""), std::string::npos) << why_text;
    EXPECT_NE(why_text.find("\"remote_missing_bytes\":0"), std::string::npos) << why_text;
#endif
}

TEST(BuildWrap, why_reports_current_policy_refusal_for_a_recorded_target) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("make") || !available("clang"))
        GTEST_SKIP() << "make or clang is absent";
    wrap_directory fixture{"make"};
    auto built = run_wrap(fixture.project(), fixture.cache(), {"make"}, false, 3);
    ASSERT_TRUE(built) << built.error().message << ": " << built.error().context;
    ASSERT_EQ(built->exit, 0);

    auto source =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(source) << source.error().message;
    auto configured = liquid::runtime::block_on(
        source->set_policy({.rules = {{.scope = liquid::policy_scope::all_inputs,
                                       .require = {{.profile = "reproducibility/1",
                                                    .from = {"unavailable-builder"},
                                                    .min_count = 1}}}}}));
    ASSERT_TRUE(configured) << configured.error().message;

    auto explanation = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), "build/wrapped", fixture.cache()));
    ASSERT_TRUE(explanation) << explanation.error().message;
    EXPECT_EQ(explanation->cause.reason, liquid::cache_miss_reason::policy_refused);
    EXPECT_FALSE(explanation->cause.path.empty());
    EXPECT_NE(explanation->cause.detail.find("config.policy.rules[0]"), std::string::npos);
}

#if defined(LIQUID_CLI)

TEST(BuildWrap, verify_and_replay_reexecute_wrapped_actions) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("make") || !available("clang"))
        GTEST_SKIP() << "make or clang is absent";
    wrap_directory fixture{"make"};
    auto cache_environment = scoped_environment{"LIQUID_WRAP_CACHE", fixture.cache().string()};
    ASSERT_TRUE(cache_environment.valid());
    auto made = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(),
                            {"make", "--silent", "--no-print-directory", "-C", "sub"},
                            {.jobs = 1, .cache = true, .verify = false}));
    ASSERT_TRUE(made) << made.error().message << ": " << made.error().context;
    ASSERT_EQ(made->exit, 0);
    const auto output = fixture.project().parent_path() / "verify-output";
    const auto command = "cd " + shell_quote(fixture.project().string()) + " && " +
                         shell_quote(LIQUID_CLI) + " verify --all >" +
                         shell_quote(output.string()) + " 2>&1";
    const auto status = std::system(command.c_str());
    auto input = std::ifstream{output};
    const auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(status, 0) << text;
    EXPECT_NE(text.find("considered=" + std::to_string(made->actions) +
                        " verified=" + std::to_string(made->actions) + " divergent=false"),
              std::string::npos)
        << text;
    ASSERT_FALSE(made->trace_actions.empty());
    const auto replay_output = fixture.project().parent_path() / "replay-output";
    const auto replay_command = "cd " + shell_quote(fixture.project().string()) + " && " +
                                shell_quote(LIQUID_CLI) + " replay " +
                                liquid::to_string(made->trace_actions.front().action) +
                                " --json >" + shell_quote(replay_output.string()) + " 2>&1";
    const auto replay_status = std::system(replay_command.c_str());
    auto replay_input = std::ifstream{replay_output};
    const auto replay_text =
        std::string{std::istreambuf_iterator<char>{replay_input}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(replay_status, 0) << replay_text;
    EXPECT_NE(replay_text.find("\"agreement\":true"), std::string::npos) << replay_text;
    EXPECT_NE(replay_text.find("\"recorded\":false"), std::string::npos) << replay_text;
}

#endif

TEST(BuildWrap, compile_database_seeds_actions) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("clang"))
        GTEST_SKIP() << "clang is absent";
    wrap_directory database_fixture{"compile-commands"};
    auto seeded = run_wrap(
        database_fixture.project(), database_fixture.cache(), {"compile_commands.json"}, false, 2);
    ASSERT_TRUE(seeded) << seeded.error().message << ": " << seeded.error().context;
    ASSERT_EQ(seeded->exit, 0);
    EXPECT_GE(seeded->processes_started, 2U);
    EXPECT_TRUE(std::filesystem::is_regular_file(database_fixture.project() / "build/left.o"));
    EXPECT_TRUE(std::filesystem::is_regular_file(database_fixture.project() / "build/right.o"));
}

TEST(BuildWrap, autoconf_probes_are_actions_and_hit_on_the_second_run) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("autoconf") || !available("clang"))
        GTEST_SKIP() << "autoconf or clang is absent";
    wrap_directory autotools_fixture{"autotools"};
    auto configured = run_wrap(autotools_fixture.project(),
                               autotools_fixture.cache(),
                               {"/bin/sh", "-c", "autoconf && ./configure"},
                               false,
                               2);
    ASSERT_TRUE(configured) << configured.error().message << ": " << configured.error().context;
    ASSERT_EQ(configured->exit, 0);
    EXPECT_GT(configured->actions, 2U);
    EXPECT_GT(configured->probes_started, 0U);
    const auto second_project = autotools_fixture.second_checkout("autotools");
    auto configured_again = run_wrap(second_project,
                                     autotools_fixture.second_cache(),
                                     {"/bin/sh", "-c", "autoconf && ./configure"},
                                     false,
                                     2);
    ASSERT_TRUE(configured_again) << configured_again.error().message << ": "
                                  << configured_again.error().context;
    EXPECT_EQ(configured_again->exit, 0);
    EXPECT_EQ(configured_again->probes_started, 0U);
}

TEST(BuildWrap, foreign_target_refuses_an_uncached_probe_before_compiler_entry) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0)
        GTEST_SKIP() << "FUSE is unavailable";
    if (!available("autoconf") || !available("clang"))
        GTEST_SKIP() << "autoconf or clang is absent";
    wrap_directory fixture{"autotools"};
    const auto marker = fixture.project() / "compiler-started";
    const auto compiler = fixture.project() / "probe-compiler";
    {
        auto output = std::ofstream{compiler};
        output << "#!/bin/sh\n";
        output << "case \"$*\" in *conftest*) touch " << shell_quote(marker.string())
               << ";; esac\n";
        output << "exec clang \"$@\"\n";
    }
    auto code = std::error_code{};
    std::filesystem::permissions(compiler,
                                 std::filesystem::perms::owner_read |
                                     std::filesystem::perms::owner_write |
                                     std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::replace,
                                 code);
    ASSERT_FALSE(code) << code.message();
    auto compiler_environment = scoped_environment{"CC", compiler.string()};
    ASSERT_TRUE(compiler_environment.valid());
#if defined(__aarch64__) || defined(__arm64__)
    const auto foreign_target = std::string{"x86_64-unknown-linux-gnu"};
#else
    const auto foreign_target = std::string{"aarch64-unknown-linux-gnu"};
#endif
    auto result = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(),
                            {"/bin/sh", "-c", "autoconf && ./configure"},
                            {.jobs = 1,
                             .cache = true,
                             .verify = false,
                             .target = foreign_target,
                             .cache_directory = fixture.cache(),
                             .fact_directory = fixture.cache().parent_path() / "facts"}));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, liquid::errc::unsupported);
    EXPECT_FALSE(std::filesystem::exists(marker));
}

#else

TEST(BuildWrap, mount_backed_scenarios_are_skipped_without_linux_fuse) {
    GTEST_SKIP() << "liquid wrap mount scenarios require Linux FUSE";
}

#endif

} // namespace
