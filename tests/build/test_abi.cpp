#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <ranges>
#include <string>
#include <vector>

#include "../temporary_directory.hpp"
#include "build/abi.hpp"
#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "liquid/abi.hpp"
#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(liquid::test::fixture_path("liquid-abi-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto read_file(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary | std::ios::ate};
    EXPECT_TRUE(input) << path;
    const auto size = input.tellg();
    EXPECT_GE(size, 0);
    auto output = std::vector<std::byte>(static_cast<std::size_t>(size));
    input.seekg(0);
    input.read(reinterpret_cast<char*>(output.data()), static_cast<std::streamsize>(output.size()));
    EXPECT_TRUE(input) << path;
    return output;
}

auto read_hex_fixture(std::string_view name) -> std::vector<std::byte> {
    auto input =
        std::ifstream{std::filesystem::path{LIQUID_ABI_FIXTURES} / (std::string{name} + ".hex")};
    EXPECT_TRUE(input) << name;
    auto output = std::vector<std::byte>{};
    auto high = -1;
    for (char character; input.get(character);) {
        const auto digit = character >= '0' && character <= '9'   ? character - '0'
                           : character >= 'a' && character <= 'f' ? character - 'a' + 10
                                                                  : -1;
        if (digit < 0) {
            continue;
        }
        if (high < 0) {
            high = digit;
        } else {
            output.push_back(std::byte{static_cast<unsigned char>((high << 4) | digit)});
            high = -1;
        }
    }
    EXPECT_EQ(high, -1);
    return output;
}

auto store_blob(liquid::repository& repository, std::vector<std::byte> contents) -> liquid::digest {
    auto chunk = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(repository, contents));
    EXPECT_TRUE(chunk) << chunk.error().message;
    auto record = liquid::objects::encode(
        liquid::blob{{*chunk}, static_cast<std::uint64_t>(contents.size()), {contents.size()}});
    EXPECT_TRUE(record) << record.error().message;
    auto stored = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(repository, std::move(*record)));
    EXPECT_TRUE(stored) << stored.error().message;
    return *stored;
}

auto store_tree(liquid::repository& repository) -> liquid::digest {
    auto encoded = liquid::objects::encode(liquid::tree{});
    EXPECT_TRUE(encoded) << encoded.error().message;
    auto stored = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(repository, std::move(*encoded)));
    EXPECT_TRUE(stored) << stored.error().message;
    return *stored;
}

auto store_build(liquid::repository& repository,
                 const std::filesystem::path& library,
                 std::chrono::seconds observed) -> liquid::action_result {
    const auto output = store_blob(repository, read_file(library));
    const auto empty = store_blob(repository, {});
    const auto tree = store_tree(repository);
    const auto action = liquid::objects::action_record{
        .input = {.command = {"cc", "-shared", "fixture.c", "-o", "libfixture.so"},
                  .outputs = {"out/libfixture"},
                  .inputs = tree,
                  .toolchain = tree,
                  .platform = {"test", "test"}},
        .sandbox = tree};
    auto value = liquid::action_result{.exit = 0,
                                       .outputs = {{"out/libfixture", 0100755, output}},
                                       .stdout_output = empty,
                                       .stderr_output = empty,
                                       .worker = "test/test",
                                       .observed_at = std::chrono::sys_seconds{observed}};
    value.finished_at = value.observed_at;
    auto stored = liquid::runtime::block_on(
        liquid::build_detail::testing::store_result_with_provenance(repository, action, value));
    EXPECT_TRUE(stored) << stored.error().message;
    return value;
}

auto symbol(const liquid::abi_descriptor& descriptor, std::string_view name)
    -> const liquid::abi_symbol* {
    const auto found =
        std::ranges::find(descriptor.exported_symbols, name, &liquid::abi_symbol::name);
    return found == descriptor.exported_symbols.end() ? nullptr : &*found;
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

TEST(Abi, extracts_the_native_shared_library_and_diffs_real_exports) {
    auto first = liquid::extract_abi(read_file(LIQUID_ABI_FIXTURE_V1));
    auto added = liquid::extract_abi(read_file(LIQUID_ABI_FIXTURE_ADDED));
    auto broken = liquid::extract_abi(read_file(LIQUID_ABI_FIXTURE_BREAK));
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(added) << added.error().message;
    ASSERT_TRUE(broken) << broken.error().message;
    EXPECT_TRUE(first->install_name);
    EXPECT_FALSE(first->architecture.empty());
    ASSERT_NE(symbol(*first, "liquid_abi_retained"), nullptr);
    ASSERT_NE(symbol(*first, "liquid_abi_removed"), nullptr);
    ASSERT_NE(symbol(*first, "liquid_abi_object"), nullptr);

    const auto additions = liquid::diff_abi(*first, *added);
    EXPECT_EQ(additions.verdict, liquid::abi_verdict::compatible_additions);
    EXPECT_TRUE(std::ranges::any_of(additions.changes, [](const auto& change) {
        return change.symbol == "liquid_abi_added" && change.kind == liquid::abi_change_kind::added;
    }));

    const auto incompatible = liquid::diff_abi(*first, *broken);
    EXPECT_EQ(incompatible.verdict, liquid::abi_verdict::incompatible);
    EXPECT_TRUE(std::ranges::any_of(incompatible.changes, [](const auto& change) {
        return change.symbol == "liquid_abi_removed" &&
               change.kind == liquid::abi_change_kind::removed;
    }));
#if defined(__linux__)
    const auto* retained = symbol(*first, "liquid_abi_retained");
    ASSERT_NE(retained, nullptr);
    EXPECT_TRUE(retained->version);
    EXPECT_EQ(*retained->version, "LIQUID_ABI_1");
    EXPECT_TRUE(retained->signature);
    EXPECT_TRUE(std::ranges::any_of(incompatible.changes, [](const auto& change) {
        return change.symbol == "liquid_abi_retained" &&
               change.kind == liquid::abi_change_kind::type_changed;
    }));
    EXPECT_TRUE(std::ranges::any_of(incompatible.changes, [](const auto& change) {
        return change.symbol == "liquid_abi_object" &&
               change.kind == liquid::abi_change_kind::size_changed;
    }));
#endif
}

TEST(Abi, reads_pinned_elf_macho_and_pe_fixtures) {
    struct expected_fixture {
        std::string_view name;
        liquid::abi_binary_kind kind;
        std::string_view architecture;
        std::string_view digest;
    };
    constexpr auto fixtures = std::array{
        expected_fixture{"elf",
                         liquid::abi_binary_kind::elf,
                         "x86_64",
                         "blake3:9e667685c37374ab7bbd0fd805400186c43a1d253febe33ea738b8829c9a36af"},
        expected_fixture{"macho",
                         liquid::abi_binary_kind::macho,
                         "arm64",
                         "blake3:642915569438a6fb0410f6e796ba7b90ac1208f9e59095a727dabca36ac836db"},
        expected_fixture{
            "pe",
            liquid::abi_binary_kind::pe,
            "x86_64",
            "blake3:4a636f1087f1adbbc990c678d2d0e28f8a583542dfc0a6bc04eea9256ab4c006"}};
    for (const auto& fixture : fixtures) {
        auto descriptor = liquid::extract_abi(read_hex_fixture(fixture.name));
        ASSERT_TRUE(descriptor) << fixture.name << ": " << descriptor.error().message;
        EXPECT_EQ(descriptor->binary, fixture.kind);
        EXPECT_EQ(descriptor->architecture, fixture.architecture);
        EXPECT_EQ(liquid::to_string(descriptor->subject), fixture.digest);
    }
    const auto macho = liquid::extract_abi(read_hex_fixture("macho"));
    const auto pe = liquid::extract_abi(read_hex_fixture("pe"));
    ASSERT_TRUE(macho);
    ASSERT_TRUE(pe);
    EXPECT_EQ(macho->install_name, "@rpath/fixture.dylib");
    EXPECT_EQ(macho->needed_libraries, std::vector<std::string>{"/usr/lib/libSystem.B.dylib"});
    EXPECT_NE(symbol(*macho, "fixture_export"), nullptr);
    EXPECT_EQ(pe->install_name, "fixture.dll");
    EXPECT_EQ(pe->needed_libraries, std::vector<std::string>{"KERNEL32.dll"});
    EXPECT_NE(symbol(*pe, "fixture_export"), nullptr);
}

TEST(Abi, stores_descriptors_and_a_verified_compatibility_attestation) {
    temporary_repository location;
    auto opened = liquid::runtime::block_on(liquid::repository::create(location.path()));
    ASSERT_TRUE(opened) << opened.error().message;
    auto repository = std::move(*opened);
    const auto first = store_build(repository, LIQUID_ABI_FIXTURE_V1, std::chrono::seconds{100});
    ASSERT_EQ(first.abis.size(), 1U);
    EXPECT_TRUE(first.abi_compatibility.empty());
    auto descriptor = liquid::runtime::block_on(repository.abi(first.outputs.front().blob));
    ASSERT_TRUE(descriptor) << descriptor.error().message;
    EXPECT_NE(symbol(*descriptor, "liquid_abi_removed"), nullptr);

    const auto broken =
        store_build(repository, LIQUID_ABI_FIXTURE_BREAK, std::chrono::seconds{200});
    ASSERT_EQ(broken.abis.size(), 1U);
    ASSERT_EQ(broken.abi_compatibility.size(), 1U);
    auto statements = liquid::runtime::block_on(
        repository.attestations({.id = broken.abi_compatibility.front()}));
    ASSERT_TRUE(statements) << statements.error().message;
    ASSERT_EQ(statements->records.size(), 1U);
    EXPECT_EQ(statements->records.front().value.profile, "abi_compatibility/1");
    auto verified = liquid::verify_attestation(statements->records.front().value);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;
    auto breaks = liquid::runtime::block_on(liquid::build_detail::abi_breaks(repository, broken));
    ASSERT_TRUE(breaks) << breaks.error().message;
    ASSERT_EQ(breaks->size(), 1U);
    EXPECT_EQ(breaks->front().output, "out/libfixture");
    EXPECT_EQ(breaks->front().report.verdict, liquid::abi_verdict::incompatible);

#if defined(LIQUID_CLI) && !defined(_WIN32)
    auto builds = liquid::runtime::block_on(
        repository.results({.output = "out/libfixture", .page = {.limit = 2}}));
    ASSERT_TRUE(builds) << builds.error().message;
    ASSERT_EQ(builds->records.size(), 2U);
    const auto old_operand = liquid::to_string(builds->records.back().id.value) + ":out/libfixture";
    const auto new_operand =
        liquid::to_string(builds->records.front().id.value) + ":out/libfixture";
    const auto output = location.path() / "abi-diff.json";
    const auto command = "cd " + shell_quote(location.path().string()) + " && " +
                         shell_quote(LIQUID_CLI) + " abi diff " + shell_quote(old_operand) + " " +
                         shell_quote(new_operand) + " --json >" + shell_quote(output.string());
    EXPECT_EQ(std::system(command.c_str()), 0);
    auto input = std::ifstream{output};
    const auto json =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    EXPECT_NE(json.find("\"verdict\":\"incompatible\""), std::string::npos) << json;
    EXPECT_NE(json.find("liquid_abi_removed"), std::string::npos) << json;
#endif
}

TEST(Abi, diff_json_is_stable) {
    auto old_descriptor = liquid::abi_descriptor{};
    old_descriptor.exported_symbols.push_back({.name = "removed",
                                               .kind = liquid::abi_symbol_kind::function,
                                               .binding = "global",
                                               .visibility = "default",
                                               .signature = "int(int)"});
    auto new_descriptor = liquid::abi_descriptor{};
    new_descriptor.exported_symbols.push_back({.name = "added",
                                               .kind = liquid::abi_symbol_kind::function,
                                               .binding = "global",
                                               .visibility = "default",
                                               .signature = "int(int)"});
    auto encoded = liquid::abi_json(liquid::diff_abi(old_descriptor, new_descriptor));
    ASSERT_TRUE(encoded) << encoded.error().message;
    EXPECT_EQ(*encoded,
              "{\"version\":1,\"verdict\":\"incompatible\",\"changes\":["
              "{\"symbol\":\"added\",\"kind\":\"added\",\"detail\":\"exported symbol was "
              "added\"},{\"symbol\":\"removed\",\"kind\":\"removed\",\"detail\":\"exported "
              "symbol was removed\"}]}");
}

} // namespace
