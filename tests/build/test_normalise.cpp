// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <tuple>
#include <vector>

#if !defined(_WIN32)
#include <sys/stat.h>
#endif

#include "../temporary_directory.hpp"
#include "build/normalise.hpp"
#include "sandbox/environment.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    std::transform(value.begin(), value.end(), output.begin(), [](char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
    });
    return output;
}

void append_field(std::string& output, std::string value, std::size_t width) {
    output += value;
    output.append(width - value.size(), ' ');
}

auto archive(std::uint64_t time, std::uint64_t uid, std::uint64_t gid) -> std::vector<std::byte> {
    auto output = std::string{"!<arch>\n"};
    append_field(output, "member.o/", 16);
    append_field(output, std::to_string(time), 12);
    append_field(output, std::to_string(uid), 6);
    append_field(output, std::to_string(gid), 6);
    append_field(output, "100644", 8);
    append_field(output, "3", 10);
    output += "`\n";
    output += "obj";
    output += '\n';
    return bytes(output);
}

auto pyc(std::uint32_t timestamp, std::uint32_t source_size) -> std::vector<std::byte> {
    auto output = bytes("PYTH");
    for (auto shift = 0U; shift != 32U; shift += 8U) {
        output.push_back(std::byte{});
    }
    for (const auto value : {timestamp, source_size}) {
        for (auto shift = 0U; shift != 32U; shift += 8U) {
            output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    }
    const auto payload = bytes("same-code-object");
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

} // namespace

TEST(OutputNormaliser, ar_member_owner_group_time_and_mode_are_deterministic) {
    auto first = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::ar_archive, archive(1, 501, 20));
    auto second = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::ar_archive, archive(9'999, 1'000, 100));

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*first, *second);
}

TEST(OutputNormaliser, python_bytecode_uses_a_payload_hash_instead_of_source_time) {
    auto first = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::python_bytecode, pyc(1, 20));
    auto second = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::python_bytecode, pyc(2, 40));

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(std::to_integer<unsigned int>((*first)[4]), 1U);
    EXPECT_EQ(std::to_integer<unsigned int>((*first)[5]), 0U);
}

TEST(OutputNormaliser, binary_and_text_kinds_need_no_byte_rewrite) {
    const auto elf = bytes("\x7f" "ELFpayload");
    const auto text = bytes("prefix=/liquid/work/fixed\n");

    EXPECT_EQ(liquid::build_detail::output_kind_for("unit.o", elf),
              liquid::build_detail::output_kind::elf_object);
    EXPECT_EQ(liquid::build_detail::output_kind_for("library.pc", text),
              liquid::build_detail::output_kind::text);
    EXPECT_EQ(*liquid::build_detail::normalise_output(
                  liquid::build_detail::output_kind::elf_object, elf),
              elf);
    EXPECT_EQ(*liquid::build_detail::normalise_output(
                  liquid::build_detail::output_kind::text, text),
              text);
}

TEST(OutputNormaliser, tree_modes_keep_only_portable_executability) {
    EXPECT_EQ(liquid::build_detail::normalised_output_mode(0100775), 0755U);
    EXPECT_EQ(liquid::build_detail::normalised_output_mode(0100660), 0644U);
}

TEST(OutputNormaliser, sandbox_rules_fix_text_and_archive_time_inputs) {
    const auto action = liquid::digest{liquid::algorithm::blake3, {}};
    const auto input = liquid::action_input{.platform = {"linux", "x86_64"}};
    auto environment = liquid::sandbox_detail::environment(
        input, action, std::chrono::sys_seconds{std::chrono::seconds{9'999}});
    ASSERT_TRUE(environment) << environment.error().message;
    EXPECT_EQ(environment->values.at("SOURCE_DATE_EPOCH"), "9999");
    EXPECT_EQ(environment->values.at("ZERO_AR_DATE"), "1");
    auto definition = liquid::sandbox_detail::definition(input.platform);
    ASSERT_TRUE(definition) << definition.error().message;
    EXPECT_EQ(definition->output_normalisers,
              liquid::build_detail::output_normaliser_rules());
    auto encoded = liquid::objects::encode(*definition);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto without_rules = *definition;
    without_rules.output_normalisers.clear();
    auto legacy = liquid::objects::encode(without_rules);
    ASSERT_TRUE(legacy) << legacy.error().message;
    EXPECT_NE(*encoded, *legacy);
}

#if !defined(_WIN32)
namespace {

// An output root, a staging directory and a host file outside both, as one action run sees them.
class output_collection {
public:
    output_collection()
        : path_(liquid::test::fixture_path("output-collection-" + std::to_string(next_++))) {
        std::filesystem::create_directories(outputs());
        std::filesystem::create_directories(staging());
        std::filesystem::create_directories(host());
        write(sentinel(), sentinel_text);
    }
    ~output_collection() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }
    output_collection(const output_collection&) = delete;
    auto operator=(const output_collection&) -> output_collection& = delete;

    auto outputs() const -> std::filesystem::path {
        return path_ / "outputs";
    }
    auto staging() const -> std::filesystem::path {
        return path_ / "staging";
    }
    auto host() const -> std::filesystem::path {
        return path_ / "host";
    }
    auto sentinel() const -> std::filesystem::path {
        return host() / "sentinel.txt";
    }

    static void write(const std::filesystem::path& path, std::string_view contents) {
        std::filesystem::create_directories(path.parent_path());
        auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
        output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    }

    static auto read(const std::filesystem::path& path) -> std::string {
        auto input = std::ifstream{path, std::ios::binary};
        return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    }

    auto staged_files() const -> std::size_t {
        return static_cast<std::size_t>(std::distance(
            std::filesystem::directory_iterator{staging()}, std::filesystem::directory_iterator{}));
    }

    static constexpr auto sentinel_text =
        std::string_view{"host sentinel that no action may read or rewrite\n"};

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto stage(const output_collection& fixture,
           std::string_view relative,
           liquid::build_detail::output_shape shape) {
    return liquid::build_detail::stage_output(
        fixture.outputs(), relative, shape, fixture.staging(), true);
}

} // namespace

TEST(OutputCollection, symlink_to_a_host_file_fails_before_the_target_is_read_or_rewritten) {
    output_collection fixture;
    output_collection::write(fixture.outputs() / "out" / "regular.txt", "regular");
    std::filesystem::create_symlink(fixture.sentinel(), fixture.outputs() / "out" / "payload.pyc");

    auto staged = stage(fixture, "out", liquid::build_detail::output_shape::directory);

    ASSERT_FALSE(staged);
    EXPECT_EQ(staged.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(staged.error().message, "action output is a symbolic link");
    EXPECT_EQ(output_collection::read(fixture.sentinel()), output_collection::sentinel_text);
    EXPECT_EQ(fixture.staged_files(), 0U);
}

TEST(OutputCollection, declared_file_that_is_a_symlink_is_refused) {
    output_collection fixture;
    std::filesystem::create_symlink(fixture.sentinel(), fixture.outputs() / "result.pyc");

    auto staged = stage(fixture, "result.pyc", liquid::build_detail::output_shape::file);

    ASSERT_FALSE(staged);
    EXPECT_EQ(staged.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(output_collection::read(fixture.sentinel()), output_collection::sentinel_text);
    EXPECT_EQ(fixture.staged_files(), 0U);
}

TEST(OutputCollection, linked_directories_are_refused_at_every_depth) {
    for (const auto& [link, declared, shape] :
         {std::tuple{"out", "out", liquid::build_detail::output_shape::directory},
          std::tuple{"out/nested", "out", liquid::build_detail::output_shape::directory},
          std::tuple{"parent", "parent/out", liquid::build_detail::output_shape::directory},
          std::tuple{"parent", "parent/file.txt", liquid::build_detail::output_shape::file}}) {
        output_collection fixture;
        output_collection::write(fixture.host() / "out" / "file.txt", "host");
        output_collection::write(fixture.host() / "file.txt", "host");
        std::filesystem::create_directories((fixture.outputs() / link).parent_path());
        std::filesystem::create_directory_symlink(fixture.host(), fixture.outputs() / link);

        auto staged = stage(fixture, declared, shape);

        ASSERT_FALSE(staged) << link;
        EXPECT_EQ(staged.error().code, liquid::errc::unauthorized) << link;
        EXPECT_EQ(fixture.staged_files(), 0U) << link;
    }
}

TEST(OutputCollection, special_files_are_refused_without_blocking) {
    output_collection fixture;
    std::filesystem::create_directories(fixture.outputs() / "out");
    ASSERT_EQ(::mkfifo((fixture.outputs() / "out" / "pipe").c_str(), 0600), 0);

    auto staged = stage(fixture, "out", liquid::build_detail::output_shape::directory);

    ASSERT_FALSE(staged);
    EXPECT_EQ(staged.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(staged.error().message, "action output is not a regular file or directory");
}

TEST(OutputCollection, escaping_declared_paths_are_refused) {
    output_collection fixture;
    for (const auto* declared : {"../host/sentinel.txt", "/etc/hosts", "out/../x", "./x", ""}) {
        auto staged = stage(fixture, declared, liquid::build_detail::output_shape::file);
        ASSERT_FALSE(staged) << declared;
        EXPECT_EQ(staged.error().code, liquid::errc::invalid_argument) << declared;
    }
}

TEST(OutputCollection, regular_outputs_are_staged_and_normalised_without_rewriting_them) {
    output_collection fixture;
    const auto bytecode = pyc(7, 70);
    const auto original =
        std::string{reinterpret_cast<const char*>(bytecode.data()), bytecode.size()};
    output_collection::write(fixture.outputs() / "out" / "a.txt", "alpha");
    output_collection::write(fixture.outputs() / "out" / "nested" / "module.pyc", original);
    std::filesystem::permissions(fixture.outputs() / "out" / "a.txt",
                                 std::filesystem::perms::owner_exec,
                                 std::filesystem::perm_options::add);

    auto staged = stage(fixture, "out", liquid::build_detail::output_shape::directory);

    ASSERT_TRUE(staged) << staged.error().message << ": " << staged.error().context;
    ASSERT_EQ(staged->size(), 2U);
    EXPECT_EQ((*staged)[0].path, "out/a.txt");
    EXPECT_NE((*staged)[0].mode & 0100U, 0U);
    EXPECT_EQ(output_collection::read((*staged)[0].staged), "alpha");
    EXPECT_EQ((*staged)[1].path, "out/nested/module.pyc");
    auto expected = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::python_bytecode, bytecode);
    ASSERT_TRUE(expected);
    EXPECT_EQ(output_collection::read((*staged)[1].staged),
              std::string(reinterpret_cast<const char*>(expected->data()), expected->size()));
    EXPECT_EQ(output_collection::read(fixture.outputs() / "out" / "nested" / "module.pyc"),
              original);
    for (const auto& file : *staged) {
        EXPECT_EQ(file.staged.parent_path(), fixture.staging());
    }

    auto single = stage(fixture, "out/a.txt", liquid::build_detail::output_shape::file);
    ASSERT_TRUE(single) << single.error().message;
    ASSERT_EQ(single->size(), 1U);
    EXPECT_EQ(single->front().path, "out/a.txt");
}

TEST(OutputCollection, missing_outputs_and_directories_declared_as_files_yield_nothing) {
    output_collection fixture;
    std::filesystem::create_directories(fixture.outputs() / "out");

    auto missing = stage(fixture, "absent/file.txt", liquid::build_detail::output_shape::file);
    auto directory = stage(fixture, "out", liquid::build_detail::output_shape::file);

    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_TRUE(missing->empty());
    ASSERT_TRUE(directory) << directory.error().message;
    EXPECT_TRUE(directory->empty());
}
#endif
