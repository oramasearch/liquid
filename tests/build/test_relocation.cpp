#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/build.hpp"
#include "liquid/runtime.hpp"

namespace {

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

void write_file(const std::filesystem::path& path, std::string_view contents) {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    ASSERT_FALSE(code) << code.message();
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(contents.data(), static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output) << path;
}

void write_file(const std::filesystem::path& path, std::span<const std::byte> contents) {
    auto code = std::error_code{};
    std::filesystem::create_directories(path.parent_path(), code);
    ASSERT_FALSE(code) << code.message();
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output.write(reinterpret_cast<const char*>(contents.data()),
                 static_cast<std::streamsize>(contents.size()));
    ASSERT_TRUE(output) << path;
}

TEST(Relocation, clean_relative_metadata_and_script_pass) {
    const auto root = liquid::test::fixture_path("liquid-relocation-clean");
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    write_file(root / "lib/pkgconfig/example.pc",
               "prefix=${pcfiledir}/../..\nlibdir=${prefix}/lib\n");
    write_file(root / "lib/cmake/example/exampleConfig.cmake",
               "set(PACKAGE_PREFIX_DIR \"${CMAKE_CURRENT_LIST_DIR}/../../..\")\n");
    write_file(root / "bin/example", "#!/usr/bin/env sh\nroot=$(dirname \"$0\")\n");

    auto report = liquid::runtime::block_on(liquid::build::relocation_check(
        root, {.install_prefix = std::filesystem::path{"/opt/example"}}));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_TRUE(report->relocatable);
    EXPECT_TRUE(report->findings.empty());
    std::filesystem::remove_all(root, code);
}

TEST(Relocation, embedded_prefix_reports_file_offset_and_context) {
    const auto root = liquid::test::fixture_path("liquid-relocation-prefix");
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    write_file(root / "share/value.txt", "before /liquid/work/action/out/prefix after\n");
    write_file(root / "share/declared.txt", "prefix=/opt/example\n");

    auto report = liquid::runtime::block_on(liquid::build::relocation_check(
        root, {.install_prefix = std::filesystem::path{"/opt/example"}}));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_FALSE(report->relocatable);
    ASSERT_FALSE(report->findings.empty());
    EXPECT_TRUE(std::ranges::any_of(report->findings, [](const auto& finding) {
        return finding.file == "share/value.txt" && finding.offset == 7U &&
               finding.context.find("virtual build root") != std::string::npos;
    }));
    EXPECT_TRUE(std::ranges::any_of(report->findings, [](const auto& finding) {
        return finding.file == "share/declared.txt" &&
               finding.context.find("declared install prefix") != std::string::npos;
    }));
    std::filesystem::remove_all(root, code);
}

TEST(Relocation, absolute_package_metadata_is_rejected) {
    const auto root = liquid::test::fixture_path("liquid-relocation-metadata");
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    write_file(root / "lib/pkgconfig/example.pc",
               "prefix=${pcfiledir}/../..\nLibs: -L/usr/local/lib -lexample\n");
    write_file(root / "lib/cmake/example/exampleConfig.cmake",
               "set(EXAMPLE_INCLUDE_DIR \"/usr/local/include\")\n");

    auto report = liquid::runtime::block_on(liquid::build::relocation_check(root));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_FALSE(report->relocatable);
    EXPECT_TRUE(std::ranges::any_of(report->findings, [](const auto& finding) {
        return finding.context == "absolute pkg-config path: Libs: -L/usr/local/lib -lexample";
    }));
    EXPECT_TRUE(std::ranges::any_of(report->findings, [](const auto& finding) {
        return finding.context == "absolute CMake package path: /usr/local/include";
    }));
    std::filesystem::remove_all(root, code);
}

TEST(Relocation, absolute_macho_install_name_is_rejected) {
    const auto root = liquid::test::fixture_path("liquid-relocation-macho");
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    auto macho = read_hex_fixture("macho");
    constexpr auto old_name = std::string_view{"@rpath/fixture.dylib"};
    constexpr auto absolute_name = std::string_view{"/tmp/xxfixture.dylib"};
    static_assert(old_name.size() == absolute_name.size());
    const auto old_bytes = std::span<const std::byte>{
        reinterpret_cast<const std::byte*>(old_name.data()), old_name.size()};
    const auto found = std::search(macho.begin(), macho.end(), old_bytes.begin(), old_bytes.end());
    ASSERT_NE(found, macho.end());
    std::transform(absolute_name.begin(), absolute_name.end(), found, [](char character) {
        return static_cast<std::byte>(character);
    });
    write_file(root / "lib/libfixture.dylib", macho);

    auto report = liquid::runtime::block_on(liquid::build::relocation_check(root));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_FALSE(report->relocatable);
    EXPECT_TRUE(std::ranges::any_of(report->findings, [](const auto& finding) {
        return finding.context == "absolute Mach-O install name: /tmp/xxfixture.dylib";
    }));
    std::filesystem::remove_all(root, code);
}

} // namespace
