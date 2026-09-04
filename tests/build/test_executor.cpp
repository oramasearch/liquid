#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <string>
#include <vector>

#include "build/repository_access.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"
#include "vfs/repository_access.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-build-test-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_repository() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto environment_entries(std::string_view value) -> std::map<std::string, std::string> {
    auto output = std::map<std::string, std::string>{};
    auto offset = std::size_t{};
    while (offset < value.size()) {
        const auto newline = value.find('\n', offset);
        const auto line = value.substr(offset, newline - offset);
        const auto separator = line.find('=');
        if (separator != std::string_view::npos) {
            output.emplace(std::string{line.substr(0, separator)},
                           std::string{line.substr(separator + 1)});
        }
        if (newline == std::string_view::npos) {
            break;
        }
        offset = newline + 1;
    }
    return output;
}

struct action_fixture {
    temporary_repository location;
    liquid::repository repository;
    liquid::digest root;

    action_fixture() {
        auto opened = liquid::runtime::block_on(liquid::repository::create(location.path()));
        EXPECT_TRUE(opened) << opened.error().message;
        repository = std::move(*opened);
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("input.txt", bytes("input"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("other.txt", bytes("other"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("out/copy.txt", {})));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("out/env.txt", {})));
        EXPECT_TRUE(
            liquid::runtime::block_on(repository.write_file("bin/fixture", LIQUID_BUILD_FIXTURE)));
        EXPECT_TRUE(liquid::runtime::block_on(
            liquid::repository_detail::vfs_access::set_mode(repository, "bin/fixture", 0100755)));
        EXPECT_TRUE(liquid::runtime::block_on(repository.cut("build fixture")));
        auto head = liquid::runtime::block_on(repository.head());
        EXPECT_TRUE(head);
        auto snapshot = liquid::runtime::block_on(repository.at(*head));
        EXPECT_TRUE(snapshot);
        auto log = liquid::runtime::block_on(repository.log());
        EXPECT_TRUE(log);
        root = log->front().root;
    }

    auto input(std::vector<std::string> command, std::vector<std::string> outputs)
        -> liquid::action_input {
        return {.command = std::move(command),
                .cwd = "",
                .env = {{"ACTION_VALUE", "present"}},
                .outputs = std::move(outputs),
                .inputs = root,
                .toolchain = root,
                .platform = host(),
                .timeout = std::chrono::seconds{3}};
    }

    static auto host() -> liquid::platform {
#if defined(__linux__)
#if defined(__aarch64__)
        return {"linux", "arm64"};
#else
        return {"linux", "x86_64"};
#endif
#elif defined(__APPLE__)
        return {"macos", "arm64"};
#else
        return {"windows", "x86_64"};
#endif
    }
};

#if LIQUID_BUILD_HAS_FUSE
auto blob_contents(liquid::repository& repository, liquid::digest id)
    -> liquid::result<std::vector<std::byte>> {
    auto record =
        liquid::runtime::block_on(liquid::repository_detail::build_access::get(repository, id));
    if (!record) {
        return std::unexpected(record.error());
    }
    auto blob = liquid::objects::decode_blob(*record);
    if (!blob) {
        return std::unexpected(blob.error());
    }
    if (blob->chunks.size() != 1U) {
        return std::unexpected(
            liquid::error{liquid::errc::corrupt, "test output has multiple chunks", "build test"});
    }
    return liquid::runtime::block_on(
        liquid::repository_detail::build_access::get(repository, blob->chunks.front()));
}

TEST(Build, copies_declared_output_and_records_only_read_input) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->exit, 0);
    ASSERT_EQ(result->outputs.size(), 1U);
    EXPECT_EQ(result->outputs.front().path, "out/copy.txt");
    ASSERT_EQ(result->manifest.size(), 1U);
    EXPECT_EQ(result->manifest.front().path, "input.txt");
}

TEST(Build, records_an_undeclared_input_read) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository,
        fixture.input({"fixture", "copy", "other.txt", "out/copy.txt"}, {"out/copy.txt"})));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    ASSERT_EQ(result->manifest.size(), 1U);
    EXPECT_EQ(result->manifest.front().path, "other.txt");
}

TEST(Build, denies_reads_outside_the_mount) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(
        liquid::build::execute(fixture.repository, fixture.input({"fixture", "outside-read"}, {})));
    ASSERT_TRUE(ran) << ran.error().message << ": " << ran.error().context;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_EQ(result->exit, 0);
    EXPECT_TRUE(result->manifest.empty());
}

TEST(Build, rejects_writes_outside_declared_outputs) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository, fixture.input({"fixture", "outside-write"}, {})));
    ASSERT_FALSE(ran);
    EXPECT_EQ(ran.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(ran.error().message, "action wrote outside declared outputs");
}

TEST(Build, has_fixed_environment) {
    action_fixture fixture;
    auto ran = liquid::runtime::block_on(liquid::build::execute(
        fixture.repository, fixture.input({"fixture", "env", "out/env.txt"}, {"out/env.txt"})));
    ASSERT_TRUE(ran) << ran.error().message;
    auto result = liquid::runtime::block_on(liquid::build::result(fixture.repository, *ran));
    ASSERT_TRUE(result);
    EXPECT_EQ(result->exit, 0);
    ASSERT_EQ(result->outputs.size(), 1U);
    auto output = blob_contents(fixture.repository, result->outputs.front().blob);
    ASSERT_TRUE(output) << output.error().message;
    auto source_time = liquid::runtime::block_on(
        liquid::repository_detail::build_access::change_time(fixture.repository, fixture.root));
    ASSERT_TRUE(source_time) << source_time.error().message;
    EXPECT_EQ(environment_entries(text(*output)),
              (std::map<std::string, std::string>{
                  {"ACTION_VALUE", "present"},
                  {"HOME", "/liquid/home/" + liquid::to_string(result->action).substr(7)},
                  {"LANG", "C.UTF-8"},
                  {"PATH", "/liquid/toolchain/" + liquid::to_string(fixture.root) + "/bin"},
                  {"SOURCE_DATE_EPOCH", std::to_string(source_time->time_since_epoch().count())},
                  {"TMPDIR", "/liquid/tmp/" + liquid::to_string(result->action).substr(7)},
                  {"TZ", "UTC"},
              }));
}

TEST(Build, preserves_pure_action_results_except_timing) {
    action_fixture fixture;
    const auto input =
        fixture.input({"fixture", "copy", "input.txt", "out/copy.txt"}, {"out/copy.txt"});
    auto first = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(first) << first.error().message;
    auto second = liquid::runtime::block_on(liquid::build::execute(fixture.repository, input));
    ASSERT_TRUE(second) << second.error().message;
    auto first_result =
        liquid::runtime::block_on(liquid::build::result(fixture.repository, *first));
    auto second_result =
        liquid::runtime::block_on(liquid::build::result(fixture.repository, *second));
    ASSERT_TRUE(first_result);
    ASSERT_TRUE(second_result);
    EXPECT_EQ(liquid::to_string(first_result->action), liquid::to_string(second_result->action));
    ASSERT_EQ(first_result->outputs.size(), second_result->outputs.size());
    EXPECT_EQ(first_result->outputs.front().path, second_result->outputs.front().path);
    EXPECT_EQ(liquid::to_string(first_result->outputs.front().blob),
              liquid::to_string(second_result->outputs.front().blob));
    ASSERT_EQ(first_result->manifest.size(), second_result->manifest.size());
    EXPECT_EQ(first_result->manifest.front().path, second_result->manifest.front().path);
    EXPECT_EQ(liquid::to_string(first_result->manifest.front().content),
              liquid::to_string(second_result->manifest.front().content));
}

TEST(Build, terminates_the_process_group_on_timeout) {
    action_fixture fixture;
    auto input = fixture.input({"fixture", "sleep"}, {});
    input.timeout = std::chrono::seconds{1};
    auto ran =
        liquid::runtime::block_on(liquid::build::execute(fixture.repository, std::move(input)));
    ASSERT_FALSE(ran);
    EXPECT_EQ(ran.error().code, liquid::errc::unavailable);
    EXPECT_EQ(ran.error().message, "action timed out");
}
#endif

} // namespace
