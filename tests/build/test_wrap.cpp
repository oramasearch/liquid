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

#include "../temporary_directory.hpp"
#include "build/cache.hpp"
#include "build/materialise.hpp"
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
        : root_(liquid::test::fixture_path("liquid-wrap-test-" + std::to_string(process_id()) +
                                           "-" + std::to_string(next_++))),
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

auto clang_command() -> std::string {
    return std::system("command -v clang >/dev/null 2>&1") == 0 ? "clang" : "clang-19";
}

auto run_wrap(const std::filesystem::path& project,
              const std::filesystem::path& cache,
              std::vector<std::string> command,
              bool explain = false,
              std::uint32_t jobs = 0,
              std::optional<liquid::remote_config> remote = {},
              std::vector<liquid::named_input> inputs = {}) -> liquid::result<liquid::wrap_report> {
    return liquid::runtime::block_on(
        liquid::build::wrap(project,
                            std::move(command),
                            liquid::wrap_options{.jobs = jobs,
                                                 .cache = true,
                                                 .explain = explain,
                                                 .verify = false,
                                                 .cache_directory = cache,
                                                 .fact_directory = cache.parent_path() / "facts",
                                                 .remote = std::move(remote),
                                                 .inputs = std::move(inputs)}));
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

liquid::runtime::task<void> edit_after_delay(std::filesystem::path path,
                                             std::chrono::milliseconds delay) {
    auto [delay_error] = co_await liquid::runtime::delay(delay);
    if (delay_error) {
        co_return std::unexpected(
            liquid::error{liquid::errc::internal, delay_error.message(), "build event test"});
    }
    auto written = co_await liquid::runtime::offload([path = std::move(path)] {
        auto output = std::ofstream{path, std::ios::app};
        output << "changed during build\n";
        if (!output) {
            return liquid::result<void>{std::unexpected(
                liquid::error{liquid::errc::io, "cannot edit build input", path.string()})};
        }
        return liquid::result<void>{};
    });
    if (!written) {
        co_return std::unexpected(written.error());
    }
    co_return std::move(*written);
}

liquid::runtime::task<std::vector<liquid::build_event>>
collect_with_edit(liquid::build_handle handle, std::filesystem::path edit) {
    auto subscription = handle.events();
    if (!subscription) {
        co_return std::unexpected(subscription.error());
    }
    liquid::runtime::spawn(edit_after_delay(std::move(edit), std::chrono::milliseconds{30}),
                           [](liquid::result<void> completed) { EXPECT_TRUE(completed); });
    auto output = std::vector<liquid::build_event>{};
    while (true) {
        auto event = co_await subscription->next();
        if (!event) {
            co_return std::unexpected(event.error());
        }
        if (!*event) {
            co_return output;
        }
        output.push_back(std::move(**event));
    }
}

liquid::runtime::task<std::vector<liquid::build_event>>
collect_events(liquid::build_handle handle) {
    auto subscription = handle.events();
    if (!subscription) {
        co_return std::unexpected(subscription.error());
    }
    auto output = std::vector<liquid::build_event>{};
    while (true) {
        auto event = co_await subscription->next();
        if (!event) {
            co_return std::unexpected(event.error());
        }
        if (!*event) {
            co_return output;
        }
        output.push_back(std::move(**event));
    }
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
    EXPECT_TRUE(std::filesystem::is_regular_file(fixture.project() / "build/CMakeCache.txt"));
    auto first =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, false, 3);
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->exit, 0);
    auto cached_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(cached_repository) << cached_repository.error().message;
    const auto imported_cache =
        liquid::runtime::block_on(cached_repository->read("build/CMakeCache.txt"));
    EXPECT_TRUE(imported_cache) << imported_cache.error().message;
    EXPECT_GE(first->processes_started, 3U);
    EXPECT_TRUE(std::filesystem::is_regular_file(fixture.project() / "build/wrapped"));

    auto repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(repository) << repository.error().message;
    auto build_plan = liquid::runtime::block_on(repository->build_plan());
    ASSERT_TRUE(build_plan) << build_plan.error().message;
    ASSERT_GE(build_plan->size(), 3U);
    const auto own_compile =
        std::ranges::find_if(*build_plan, [](const liquid::build_plan_action& action) {
            return std::ranges::any_of(action.input.command, [](const std::string& argument) {
                return std::filesystem::path{argument}.filename() == "left.c";
            });
        });
    ASSERT_NE(own_compile, build_plan->end());
    EXPECT_FALSE(own_compile->input.outputs.empty());
    EXPECT_TRUE(std::ranges::any_of(*build_plan, [](const liquid::build_plan_action& action) {
        return !action.dependencies.empty();
    }));
    for (const auto& planned : *build_plan) {
        EXPECT_FALSE(planned.input.command.empty());
        EXPECT_FALSE(planned.input.outputs.empty());
        EXPECT_FALSE(planned.input.platform.os.empty());
        EXPECT_FALSE(planned.input.platform.arch.empty());
        auto inputs = liquid::runtime::block_on(repository->tree(planned.input.inputs));
        EXPECT_TRUE(inputs) << inputs.error().message;
        auto toolchain = liquid::runtime::block_on(repository->tree(planned.input.toolchain));
        EXPECT_TRUE(toolchain) << toolchain.error().message;
    }
    auto tests = liquid::runtime::block_on(repository->tests());
    ASSERT_TRUE(tests) << tests.error().message;
    ASSERT_EQ(tests->size(), 1U);
    EXPECT_EQ(tests->front().name, "wrapped-smoke");
    EXPECT_EQ(tests->front().labels, (std::vector<std::string>{"quick", "wrapped"}));
    auto test_executable = liquid::runtime::block_on(repository->read("build/wrapped"));
    ASSERT_TRUE(test_executable) << test_executable.error().message;
    auto first_test = repository->run_tests("quick");
    ASSERT_TRUE(first_test) << first_test.error().message;
    auto first_test_events = liquid::runtime::block_on(collect_events(std::move(*first_test)));
    ASSERT_TRUE(first_test_events) << first_test_events.error().message;
    const auto first_test_result =
        std::ranges::find_if(*first_test_events, [](const liquid::build_event& event) {
            return event.test.has_value();
        });
    ASSERT_NE(first_test_result, first_test_events->end());
    EXPECT_EQ(first_test_result->test->status, liquid::test_status::passed)
        << first_test_result->test->output;
    EXPECT_FALSE(first_test_result->test->hit);
    auto second_test = repository->run_tests("wrapped-smoke");
    ASSERT_TRUE(second_test) << second_test.error().message;
    auto second_test_events = liquid::runtime::block_on(collect_events(std::move(*second_test)));
    ASSERT_TRUE(second_test_events) << second_test_events.error().message;
    const auto second_test_result =
        std::ranges::find_if(*second_test_events, [](const liquid::build_event& event) {
            return event.test.has_value();
        });
    ASSERT_NE(second_test_result, second_test_events->end());
    EXPECT_TRUE(second_test_result->test->hit);
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
    auto compiler_action_bytes = liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(*repository, left_before_result->action));
    ASSERT_TRUE(compiler_action_bytes) << compiler_action_bytes.error().message;
    auto compiler_action = liquid::objects::decode_action(*compiler_action_bytes);
    ASSERT_TRUE(compiler_action) << compiler_action.error().message;
    auto toolchain = liquid::runtime::block_on(repository->tree(compiler_action->input.toolchain));
    ASSERT_TRUE(toolchain) << toolchain.error().message;
    const auto bin = std::ranges::find(toolchain->entries, "bin", &liquid::tree_entry::name);
    const auto lib = std::ranges::find(toolchain->entries, "lib", &liquid::tree_entry::name);
    ASSERT_NE(bin, toolchain->entries.end());
    ASSERT_NE(lib, toolchain->entries.end());
    auto compiler_bin = liquid::runtime::block_on(repository->tree(bin->ref));
    ASSERT_TRUE(compiler_bin) << compiler_bin.error().message;
    const auto compiler_name =
        std::filesystem::path{compiler_action->input.command.front()}.filename();
    EXPECT_NE(
        std::ranges::find(compiler_bin->entries, compiler_name.string(), &liquid::tree_entry::name),
        compiler_bin->entries.end());
    auto compiler_libraries = liquid::runtime::block_on(repository->tree(lib->ref));
    ASSERT_TRUE(compiler_libraries) << compiler_libraries.error().message;
    EXPECT_TRUE(std::ranges::any_of(compiler_libraries->entries, [](const auto& entry) {
        return entry.name.starts_with("libclang-cpp") || entry.name.starts_with("libLLVM");
    }));
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

TEST(BuildWrap, unchanged_upstream_output_terminates_a_cross_project_cascade) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    if (!available("clang")) {
        GTEST_SKIP() << "clang is absent";
    }
    wrap_directory fixture{"cmake"};
    auto code = std::error_code{};
    std::filesystem::remove_all(fixture.project(), code);
    ASSERT_FALSE(code) << code.message();
    std::filesystem::create_directories(fixture.project(), code);
    ASSERT_FALSE(code) << code.message();
    {
        auto source = std::ofstream{fixture.project() / "value.c"};
        source << "int dependency_value(void) { return 7; }\n";
    }
    const auto compiler = clang_command();
    const auto compile = compiler + " -c value.c -o value.o";
    auto first = run_wrap(fixture.project(), fixture.cache(), {"/bin/sh", "-c", compile});
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->processes_started, 1U);
    ASSERT_TRUE(first->output_tree);
    auto upstream_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(upstream_repository) << upstream_repository.error().message;
    auto upstream_cache = liquid::build_detail::disk_cache{*upstream_repository};
    ASSERT_TRUE(liquid::runtime::block_on(upstream_cache.open()));
    auto first_result = liquid::runtime::block_on(upstream_cache.find_output("value.o"));
    ASSERT_TRUE(first_result) << first_result.error().message;
    ASSERT_TRUE(*first_result);

    const auto downstream = fixture.second_checkout();
    std::filesystem::remove_all(downstream, code);
    ASSERT_FALSE(code) << code.message();
    std::filesystem::create_directories(downstream, code);
    ASSERT_FALSE(code) << code.message();
    auto consumed = run_wrap(downstream,
                             fixture.second_cache(),
                             {"/bin/sh", "-c", "cp deps/upstream/value.o copied.o"},
                             false,
                             0,
                             {},
                             {{"upstream", (**first_result).value}});
    ASSERT_TRUE(consumed) << consumed.error().message << ": " << consumed.error().context;
    ASSERT_GT(consumed->processes_started, 0U);

    {
        auto source = std::ofstream{fixture.project() / "value.c", std::ios::app};
        source << "// output-preserving source change\n";
    }
    auto rebuilt = run_wrap(fixture.project(), fixture.cache(), {"/bin/sh", "-c", compile});
    ASSERT_TRUE(rebuilt) << rebuilt.error().message << ": " << rebuilt.error().context;
    ASSERT_EQ(rebuilt->processes_started, 1U);
    ASSERT_TRUE(rebuilt->output_tree);
    EXPECT_EQ(liquid::to_string(*rebuilt->output_tree), liquid::to_string(*first->output_tree));
    auto rebuilt_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(rebuilt_repository) << rebuilt_repository.error().message;
    auto rebuilt_cache = liquid::build_detail::disk_cache{*rebuilt_repository};
    ASSERT_TRUE(liquid::runtime::block_on(rebuilt_cache.open()));
    auto second_result = liquid::runtime::block_on(rebuilt_cache.find_output("value.o"));
    ASSERT_TRUE(second_result) << second_result.error().message;
    ASSERT_TRUE(*second_result);
    EXPECT_NE(liquid::to_string((**second_result).value),
              liquid::to_string((**first_result).value));

    auto terminated = run_wrap(downstream,
                               fixture.second_cache(),
                               {"/bin/sh", "-c", "cp deps/upstream/value.o copied.o"},
                               false,
                               0,
                               {},
                               {{"upstream", (**second_result).value}});
    ASSERT_TRUE(terminated) << terminated.error().message << ": " << terminated.error().context;
    EXPECT_EQ(terminated->processes_started, 0U);
    EXPECT_EQ(terminated->outer, liquid::cache_status::hit);
    auto explanation = liquid::runtime::block_on(
        liquid::build::why(downstream, "copied.o", fixture.second_cache()));
    ASSERT_TRUE(explanation) << explanation.error().message;
    EXPECT_EQ(explanation->cause.detail,
              "input deps/upstream unchanged: same normalised tree after upstream rebuild");
    auto downstream_repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.second_cache() / "repository"));
    ASSERT_TRUE(downstream_repository) << downstream_repository.error().message;
    auto claims = liquid::runtime::block_on(liquid::build::known_outputs(*downstream_repository));
    ASSERT_TRUE(claims) << claims.error().message;
    const auto upstream_claims = std::ranges::count_if(*claims, [&](const auto& claim) {
        return liquid::to_string(claim.output_tree) == liquid::to_string(*first->output_tree);
    });
    EXPECT_GE(upstream_claims, 2);
}

TEST(BuildWrap, incompatible_shared_library_is_reported_and_can_fail_the_cli) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    if (!available("cmake") || !available("ninja") || !available("clang")) {
        GTEST_SKIP() << "cmake, ninja, or clang is absent";
    }
    wrap_directory fixture{"abi"};
    auto cache_environment = scoped_environment{"LIQUID_WRAP_CACHE", fixture.cache().string()};
    ASSERT_TRUE(cache_environment.valid());
    auto configured = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(),
                            {"cmake", "-S", ".", "-B", "build", "-G", "Ninja"},
                            {.cache = true, .verify = false}));
    ASSERT_TRUE(configured) << configured.error().message << ": " << configured.error().context;
    ASSERT_EQ(configured->exit, 0);
    auto first = liquid::runtime::block_on(liquid::build::wrap(
        fixture.project(), {"cmake", "--build", "build"}, {.cache = true, .verify = false}));
    ASSERT_TRUE(first) << first.error().message << ": " << first.error().context;
    ASSERT_EQ(first->exit, 0);
    EXPECT_TRUE(first->abi_breaks.empty());

    auto source = std::ofstream{fixture.project() / "abi.c", std::ios::trunc};
    source << "int abi_retained(double value) { return (int)value + 1; }\n";
    source.close();
#if defined(LIQUID_CLI)
    const auto output = fixture.project().parent_path() / "abi-break-output";
    const auto command = "cd " + shell_quote(fixture.project().string()) + " && " +
                         shell_quote(LIQUID_CLI) +
                         " wrap --fail-on-abi-break cmake --build build --clean-first >" +
                         shell_quote(output.string()) + " 2>&1";
    const auto status = std::system(command.c_str());
    auto input = std::ifstream{output};
    const auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    ASSERT_TRUE(WIFEXITED(status)) << text;
    EXPECT_EQ(WEXITSTATUS(status), 1) << text;
    EXPECT_NE(text.find("abi-break:"), std::string::npos) << text;
    EXPECT_NE(text.find("abi_removed removed"), std::string::npos) << text;
    EXPECT_NE(text.find("abi_retained type_changed"), std::string::npos) << text;
#endif
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

TEST(BuildEvents, edit_during_build_replans_and_reports_untouched_hits) {
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
        auto left = std::ofstream{fixture.project() / "event-left.txt"};
        left << "left\n";
        auto right = std::ofstream{fixture.project() / "event-right.txt"};
        right << "right\n";
    }
    const auto script = "./sleep-tool event-left.txt event-left.out 150 && "
                        "./sleep-tool event-right.txt event-right.out 150";
    auto warmed = run_wrap(fixture.project(), fixture.cache(), {"/bin/sh", "-c", script}, false, 2);
    ASSERT_TRUE(warmed) << warmed.error().message << ": " << warmed.error().context;
    {
        auto left = std::ofstream{fixture.project() / "event-left.txt", std::ios::app};
        left << "before build\n";
    }
    auto handle =
        liquid::build::start_wrap(fixture.project(),
                                  {"/bin/sh", "-c", script},
                                  {.jobs = 2,
                                   .cache = true,
                                   .verify = false,
                                   .cache_directory = fixture.cache(),
                                   .fact_directory = fixture.cache().parent_path() / "facts"});
    ASSERT_TRUE(handle) << handle.error().message;
    auto events = liquid::runtime::block_on(
        collect_with_edit(std::move(*handle), fixture.project() / "event-right.txt"));
    ASSERT_TRUE(events) << events.error().message;
    const auto hits = std::ranges::count(
        *events, liquid::build_event_kind::action_hit, &liquid::build_event::kind);
    EXPECT_GE(hits, 2);
    EXPECT_EQ(events->back().kind, liquid::build_event_kind::build_finished);
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

TEST(BuildPlan, cli_prints_configured_compile_and_link_actions) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    if (!available("make") || !available("clang")) {
        GTEST_SKIP() << "make or clang is absent";
    }
    wrap_directory fixture{"make"};
    auto cache_environment =
        scoped_environment{"LIQUID_WRAP_CACHE", (fixture.cache().parent_path() / "plan").string()};
    ASSERT_TRUE(cache_environment.valid());
    const auto output = fixture.cache().parent_path() / "build-plan.jsonl";
    const auto events = fixture.cache().parent_path() / "repository-build.jsonl";
    const auto command =
        "cd " + shell_quote(fixture.project().string()) + " && " + shell_quote(LIQUID_CLI) +
        " wrap make >/dev/null && " + shell_quote(LIQUID_CLI) + " build --plan >" +
        shell_quote(output.string()) + " && " + shell_quote(LIQUID_CLI) + " build build/wrapped >" +
        shell_quote(events.string()) + "; result=$?; " + shell_quote(LIQUID_CLI) +
        " daemon --shutdown >/dev/null 2>&1 || true; exit $result";
    const auto status = std::system(command.c_str());
    ASSERT_EQ(status, 0);
    auto input = std::ifstream{output};
    ASSERT_TRUE(input.is_open());
    auto lines = std::uint64_t{};
    auto compile = false;
    auto dependent = false;
    for (auto line = std::string{}; std::getline(input, line);) {
        auto json = liquid::formats::json::parse(line);
        ASSERT_TRUE(json) << line;
        EXPECT_NE(line.find("\"action\":"), std::string::npos) << line;
        EXPECT_NE(line.find("\"state\":"), std::string::npos) << line;
        EXPECT_NE(line.find("\"command\":"), std::string::npos) << line;
        EXPECT_NE(line.find("\"inputs\":"), std::string::npos) << line;
        EXPECT_NE(line.find("\"toolchain\":"), std::string::npos) << line;
        EXPECT_NE(line.find("\"platform\":"), std::string::npos) << line;
        compile = compile || line.find(".c\"") != std::string::npos;
        dependent = dependent || line.find("\"dependencies\":[]") == std::string::npos;
        ++lines;
    }
    EXPECT_GE(lines, 4U);
    EXPECT_TRUE(compile);
    EXPECT_TRUE(dependent);

    auto event_input = std::ifstream{events};
    ASSERT_TRUE(event_input.is_open());
    auto event_lines = std::vector<std::string>{};
    for (auto line = std::string{}; std::getline(event_input, line);) {
        auto json = liquid::formats::json::parse(line);
        ASSERT_TRUE(json) << line;
        event_lines.push_back(std::move(line));
    }
    ASSERT_GE(event_lines.size(), 5U);
    EXPECT_NE(event_lines.front().find("\"type\":\"action_queued\""), std::string::npos);
    EXPECT_NE(event_lines.back().find("\"type\":\"build_finished\""), std::string::npos);

    auto repository = liquid::runtime::block_on(liquid::repository::open_build(fixture.project()));
    ASSERT_TRUE(repository) << repository.error().message;
    auto local = repository->build(liquid::build_target{"build/wrapped"});
    ASSERT_TRUE(local) << local.error().message;
    auto local_events = liquid::runtime::block_on(collect_events(std::move(*local)));
    ASSERT_TRUE(local_events) << local_events.error().message;
    ASSERT_EQ(local_events->size(), event_lines.size());
    for (auto index = std::size_t{}; index != local_events->size(); ++index) {
        auto json = liquid::event_json((*local_events)[index]);
        ASSERT_TRUE(json) << json.error().message;
        const auto type = json->find("\"type\":");
        const auto type_end = json->find(',', type);
        ASSERT_NE(type, std::string::npos);
        ASSERT_NE(type_end, std::string::npos);
        EXPECT_NE(event_lines[index].find(json->substr(type, type_end - type)), std::string::npos);
    }
}

TEST(BuildEvents, cli_prints_json_lines_and_preserves_failure_exit) {
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
    wrap_directory fixture{"make"};
    auto cache_environment =
        scoped_environment{"LIQUID_WRAP_CACHE", (fixture.cache().parent_path() / "cli").string()};
    ASSERT_TRUE(cache_environment.valid());
    const auto output = fixture.cache().parent_path() / "build-events.jsonl";
    const auto command = "cd " + shell_quote(fixture.project().string()) + " && " +
                         shell_quote(LIQUID_CLI) + " build --events /bin/sh -c " +
                         shell_quote("exit 7") + " >" + shell_quote(output.string());
    const auto status = std::system(command.c_str());
    EXPECT_EQ(status, 7 * 256);
    auto input = std::ifstream{output};
    auto lines = std::uint64_t{};
    auto failed = false;
    auto finished = false;
    for (auto line = std::string{}; std::getline(input, line);) {
        auto json = liquid::formats::json::parse(line);
        ASSERT_TRUE(json) << line;
        ++lines;
        failed = failed || line.find("\"type\":\"action_failed\"") != std::string::npos;
        finished = finished || line.find("\"type\":\"build_finished\"") != std::string::npos;
    }
    EXPECT_GE(lines, 4U);
    EXPECT_TRUE(failed);
    EXPECT_TRUE(finished);
}

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
    const auto graph_output = fixture.project().parent_path() / "graph-verify-output";
    const auto graph_command = "cd " + shell_quote(fixture.project().string()) + " && " +
                               shell_quote(LIQUID_CLI) + " verify --graph --all >" +
                               shell_quote(graph_output.string()) + " 2>&1";
    const auto graph_status = std::system(graph_command.c_str());
    auto graph_input = std::ifstream{graph_output};
    const auto graph_text =
        std::string{std::istreambuf_iterator<char>{graph_input}, std::istreambuf_iterator<char>{}};
    ASSERT_EQ(graph_status, 0) << graph_text;
    EXPECT_NE(graph_text.find("graph_divergent=0 backlog=0"), std::string::npos) << graph_text;
    EXPECT_NE(graph_text.find("actions_verified=" + std::to_string(made->actions)),
              std::string::npos)
        << graph_text;
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

#if LIQUID_BUILD_HAS_FUSE && (defined(__linux__) || defined(__APPLE__))

TEST(BuildWrap, installed_cmake_tree_moves_and_embedded_prefix_policy_is_recorded) {
#if defined(__linux__)
    if (::access("/dev/fuse", R_OK | W_OK) != 0) {
        GTEST_SKIP() << "FUSE is unavailable";
    }
#endif
    if (!available("cmake") || !available("ninja") || !available("clang")) {
        GTEST_SKIP() << "cmake, ninja, or clang is absent";
    }
    wrap_directory fixture{"relocation"};
    const auto prefix = std::filesystem::path{"/opt/liquid-relocation"};
    const auto options = [&](bool allow = false) {
        return liquid::wrap_options{.cache = true,
                                    .verify = false,
                                    .cache_directory = fixture.cache(),
                                    .fact_directory = fixture.cache().parent_path() / "facts",
                                    .install_prefix = prefix,
                                    .allow_embedded_prefix = allow};
    };
    auto configured = liquid::runtime::block_on(liquid::build::wrap(
        fixture.project(), {"cmake", "-S", ".", "-B", "build", "-G", "Ninja"}, options()));
    ASSERT_TRUE(configured) << configured.error().message << ": " << configured.error().context;
    ASSERT_EQ(configured->exit, 0);
    auto built = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(), {"cmake", "--build", "build"}, options()));
    ASSERT_TRUE(built) << built.error().message << ": " << built.error().context;
    ASSERT_EQ(built->exit, 0);
    auto installed = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(), {"cmake", "--install", "build"}, options()));
    ASSERT_TRUE(installed) << installed.error().message << ": " << installed.error().context;
    ASSERT_EQ(installed->exit, 0);
    ASSERT_TRUE(installed->output_tree);

    auto repository =
        liquid::runtime::block_on(liquid::repository::open(fixture.cache() / "repository"));
    ASSERT_TRUE(repository) << repository.error().message;
    const auto first = fixture.project().parent_path() / "installed-x";
    const auto second = fixture.project().parent_path() / "installed-y";
    auto materialised = liquid::runtime::block_on(
        liquid::build_detail::materialise_tree(*repository, *installed->output_tree, first));
    ASSERT_TRUE(materialised) << materialised.error().message;
    auto code = std::error_code{};
    std::filesystem::copy(first, second, std::filesystem::copy_options::recursive, code);
    ASSERT_FALSE(code) << code.message();
    std::filesystem::remove_all(first, code);
    ASSERT_FALSE(code) << code.message();
    const auto program_output = fixture.project().parent_path() / "relocation-output";
    const auto run = shell_quote((second / "bin/relocation-tool").string()) + " >" +
                     shell_quote(program_output.string());
    ASSERT_EQ(std::system(run.c_str()), 0);
    auto input = std::ifstream{program_output};
    auto text = std::string{};
    std::getline(input, text);
    EXPECT_EQ(text, "73");
    auto clean = liquid::runtime::block_on(
        liquid::build::relocation_check(second, {.install_prefix = prefix}));
    ASSERT_TRUE(clean) << clean.error().message;
    EXPECT_TRUE(clean->relocatable);

    const auto installed_relative = "out/prefix/opt/liquid-relocation/share/embedded.txt";
    const auto dirty_command = std::vector<std::string>{
        "/bin/sh",
        "-c",
        "mkdir -p out/prefix/opt/liquid-relocation/share && printf '/opt/liquid-relocation\\n' "
        "> out/prefix/opt/liquid-relocation/share/embedded.txt"};
    auto waived = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(), dirty_command, options(true)));
    ASSERT_TRUE(waived) << waived.error().message << ": " << waived.error().context;
    auto explanation = liquid::runtime::block_on(
        liquid::build::why(fixture.project(), installed_relative, fixture.cache()));
    ASSERT_TRUE(explanation) << explanation.error().message;
    ASSERT_TRUE(explanation->schedule);
    EXPECT_NE(explanation->schedule->reason.find("relocation check waived"), std::string::npos);

    auto rejected_command = dirty_command;
    rejected_command.back() =
        "printf '/liquid/work/rejected\\n' > " + std::string{installed_relative};
    auto rejected = liquid::runtime::block_on(
        liquid::build::wrap(fixture.project(), std::move(rejected_command), options()));
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().code, liquid::errc::invalid_argument);
    EXPECT_NE(rejected.error().message.find("embedded.txt@"), std::string::npos);
}

#endif

} // namespace
