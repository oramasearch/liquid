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

#include "liquid/build.hpp"
#include "liquid/runtime.hpp"

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

    auto second_checkout() const -> std::filesystem::path {
        const auto path = root_ / "second-project";
        auto code = std::error_code{};
        std::filesystem::copy(std::filesystem::path{LIQUID_WRAP_FIXTURES} / "cmake",
                              path,
                              std::filesystem::copy_options::recursive,
                              code);
        EXPECT_FALSE(code) << code.message();
        return path;
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
              std::uint32_t jobs = 0) -> liquid::result<liquid::wrap_report> {
    return liquid::runtime::block_on(
        liquid::build::wrap(project,
                            std::move(command),
                            liquid::wrap_options{.jobs = jobs,
                                                 .cache = true,
                                                 .explain = explain,
                                                 .verify = false,
                                                 .cache_directory = cache}));
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

    auto second =
        run_wrap(fixture.project(), fixture.cache(), {"cmake", "--build", "build"}, false, 3);
    ASSERT_TRUE(second) << second.error().message << ": " << second.error().context;
    EXPECT_EQ(second->exit, 0);
    EXPECT_EQ(second->processes_started, 0U);
    EXPECT_EQ(second->outer, liquid::cache_status::hit);

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

#if defined(LIQUID_CLI)

TEST(BuildWrap, verify_all_reexecutes_every_wrapped_action) {
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
    auto configured_again = run_wrap(autotools_fixture.project(),
                                     autotools_fixture.cache(),
                                     {"/bin/sh", "-c", "autoconf && ./configure"},
                                     false,
                                     2);
    ASSERT_TRUE(configured_again) << configured_again.error().message;
    EXPECT_EQ(configured_again->outer, liquid::cache_status::hit);
}

#else

TEST(BuildWrap, mount_backed_scenarios_are_skipped_without_linux_fuse) {
    GTEST_SKIP() << "liquid wrap mount scenarios require Linux FUSE";
}

#endif

} // namespace
