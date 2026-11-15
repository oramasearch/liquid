// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "liquid/configuration.hpp"
#include "liquid/import.hpp"
#include "liquid/resolve.hpp"
#include "liquid/runtime.hpp"

namespace {

auto fixture(std::string_view name) -> std::filesystem::path {
    return std::filesystem::path{LIQUID_SOURCE_ROOT} / "tests/import/fixtures" / name;
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, {}};
}

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto patch_bytes() -> std::vector<std::byte> {
    constexpr auto value = std::string_view{"diff --git a/zlib.c b/zlib.c\n"};
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto options() -> liquid::spack_import_options {
    return {.patch_contents = {
                {"https://example.invalid/zlib-relocation.patch", patch_bytes()},
                {"https://github.com/ROCm/HIP/commit/"
                 "50ee82f6bc4aad10908ce09198c9f7ebfb2a3561.patch?full_index=1",
                 bytes(read_text(fixture("hip.patch")))},
            }};
}

auto imported_documents(const std::filesystem::path& root) -> std::vector<std::filesystem::path> {
    auto output = std::vector<std::filesystem::path>{};
    for (const auto& entry : std::filesystem::recursive_directory_iterator{root / "packages"}) {
        if (entry.is_regular_file() && entry.path().extension() == ".liquid") {
            output.push_back(entry.path());
        }
    }
    std::ranges::sort(output);
    return output;
}

TEST(SpackImport, twenty_packages_match_golden_documents_and_report) {
    const auto output = liquid::test::fixture_path("spack-import-twenty");
    auto report = liquid::runtime::block_on(
        liquid::import_spack(fixture("spack-20.json"), output, options()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->packages, 20U);
    EXPECT_EQ(report->pinned_patches, 1U);
    EXPECT_GT(report->translated_fields, 20U);
    EXPECT_GT(report->losses, 2U);

    const auto documents = imported_documents(output);
    ASSERT_EQ(documents.size(), 20U);
    for (const auto& document : documents) {
        const auto relative = std::filesystem::relative(document, output / "packages");
        const auto golden = fixture("golden/packages") / relative;
        ASSERT_TRUE(std::filesystem::is_regular_file(golden)) << golden;
        EXPECT_EQ(read_text(document), read_text(golden)) << relative;
        auto parsed = liquid::parse_configuration(read_text(document), document);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_TRUE(liquid::check(*parsed).empty()) << relative;
    }
    EXPECT_EQ(read_text(output / "import-report.json"),
              read_text(fixture("golden/import-report.json")));
    auto decoded = liquid::decode_spack_import_report(read_text(output / "import-report.json"));
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(*decoded, *report);

    auto universe = liquid::runtime::block_on(liquid::load_package_universe(output));
    ASSERT_TRUE(universe) << universe.error().message;
    ASSERT_EQ(universe->packages.size(), 20U);
    const auto hdf5 = std::ranges::find(universe->packages, "hdf5", &liquid::package_record::name);
    ASSERT_NE(hdf5, universe->packages.end());
    ASSERT_FALSE(hdf5->versions.empty());
    EXPECT_TRUE(hdf5->versions.front().source.has_value());
    EXPECT_EQ(hdf5->variants.front().name, "mpi");
}

TEST(SpackImport, five_hundred_package_closure_resolves_and_explains_conflict) {
    const auto output = liquid::test::fixture_path("spack-import-five-hundred");
    auto report = liquid::runtime::block_on(
        liquid::import_spack(fixture("spack-500.json"), output, options()));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report->packages, 500U);
    auto universe = liquid::runtime::block_on(liquid::load_package_universe(output));
    ASSERT_TRUE(universe) << universe.error().message;
    ASSERT_EQ(universe->packages.size(), 500U);

    auto roots = std::vector<liquid::spec>{};
    for (const auto& root : report->roots) {
        auto parsed = liquid::parse_spec(root);
        ASSERT_TRUE(parsed) << parsed.error().message;
        roots.push_back(std::move(*parsed));
    }
    auto solved = liquid::runtime::block_on(liquid::resolve(*universe, std::move(roots)));
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    const auto* root_conflict = std::get_if<liquid::unsatisfiable>(&*solved);
    ASSERT_NE(graph, nullptr) << (root_conflict ? root_conflict->narrative : "unexpected result");
    EXPECT_EQ(graph->nodes.size(), 500U);

    auto conflict = liquid::parse_spec("hdf5~mpi ^openmpi");
    ASSERT_TRUE(conflict) << conflict.error().message;
    auto failed = liquid::runtime::block_on(liquid::resolve(*universe, {*conflict}));
    ASSERT_TRUE(failed) << failed.error().message;
    const auto* unsatisfiable = std::get_if<liquid::unsatisfiable>(&*failed);
    ASSERT_NE(unsatisfiable, nullptr);
    EXPECT_NE(unsatisfiable->narrative.find("hdf5"), std::string::npos);
    EXPECT_NE(unsatisfiable->narrative.find("conflict"), std::string::npos);
}

TEST(SpackImport, pinned_spack_checkout_export_imports_more_than_five_hundred_packages) {
    const auto output = liquid::test::fixture_path("spack-import-pinned-export");
    auto report = liquid::runtime::block_on(
        liquid::import_spack(fixture("spack-560.json"), output, options()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->packages, 540U);
    EXPECT_EQ(report->pinned_patches, 1U);
    EXPECT_GT(report->losses, 4'000U);
    auto universe = liquid::runtime::block_on(liquid::load_package_universe(output));
    ASSERT_TRUE(universe) << universe.error().message;
    EXPECT_EQ(universe->packages.size(), 540U);
}

} // namespace
