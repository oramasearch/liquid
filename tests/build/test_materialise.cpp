#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "build/materialise.hpp"
#include "build/repository_access.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

class materialise_fixture {
public:
    materialise_fixture()
        : root_(liquid::test::fixture_path("liquid-build-materialise-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
        std::filesystem::create_directories(root_, code);
        EXPECT_FALSE(code) << code.message();
        auto opened = liquid::runtime::block_on(liquid::repository::create(root_ / "repository"));
        EXPECT_TRUE(opened) << opened.error().message;
        source = std::move(*opened);
    }

    ~materialise_fixture() {
        auto code = std::error_code{};
        std::filesystem::remove_all(root_, code);
    }

    [[nodiscard]] auto path(std::string_view name) const -> std::filesystem::path {
        return root_ / name;
    }

    liquid::repository source;

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path root_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

// Replaces the CBOR text string `from` with `to` in an encoded record. Both are shorter than 24
// bytes; CBOR containers count items, so the record stays well formed.
auto rename_in_record(std::vector<std::byte> record, std::string_view from, std::string_view to)
    -> std::vector<std::byte> {
    auto text = [](std::string_view value) {
        auto output =
            std::vector<std::byte>{std::byte{static_cast<unsigned char>(0x60 + value.size())}};
        for (const auto item : value) {
            output.push_back(static_cast<std::byte>(item));
        }
        return output;
    };
    const auto search = text(from);
    const auto replacement = text(to);
    const auto found = std::ranges::search(record, search);
    EXPECT_FALSE(found.empty());
    const auto offset = std::distance(record.begin(), found.begin());
    record.erase(found.begin(), found.end());
    record.insert(record.begin() + offset, replacement.begin(), replacement.end());
    return record;
}

auto case_insensitive(const std::filesystem::path& directory) -> bool {
    auto code = std::error_code{};
    std::filesystem::create_directories(directory, code);
    {
        auto output = std::ofstream{directory / "case-probe"};
    }
    return std::filesystem::exists(directory / "CASE-PROBE", code);
}

// liquid #386: a stored tree whose entry name climbs out of the destination is refused, and
// nothing is written outside it.
TEST(BuildMaterialise, refuses_a_tree_entry_name_that_escapes_the_destination) {
    materialise_fixture fixture;
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("placeholder", bytes("escaped"))));
    auto tree = liquid::runtime::block_on(fixture.source.root());
    ASSERT_TRUE(tree) << tree.error().message;
    auto encoded = liquid::objects::encode(*tree);
    ASSERT_TRUE(encoded);
    for (const auto* name : {"../escaped", "..", ".git"}) {
        auto crafted = rename_in_record(*encoded, "placeholder", name);
        auto root = liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
            fixture.source, std::move(crafted)));
        ASSERT_TRUE(root) << root.error().message;
        const auto destination =
            fixture.path("out") / std::to_string(std::hash<std::string>{}(name));
        auto materialised = liquid::runtime::block_on(
            liquid::build_detail::materialise_tree(fixture.source, *root, destination));
        ASSERT_FALSE(materialised) << "accepted " << name;
        EXPECT_EQ(materialised.error().code, liquid::errc::corrupt);
        EXPECT_FALSE(std::filesystem::exists(fixture.path("out") / "escaped"));
        EXPECT_FALSE(std::filesystem::exists(destination / ".git"));
    }
}

TEST(BuildMaterialise, names_that_collide_on_this_filesystem_are_refused) {
    materialise_fixture fixture;
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("README", bytes("upper"))));
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("Readme", bytes("mixed"))));
    auto root = liquid::runtime::block_on(fixture.source.root_digest());
    ASSERT_TRUE(root) << root.error().message;
    const auto destination = fixture.path("collide");
    const auto folds_case = case_insensitive(fixture.path("probe"));
    auto materialised = liquid::runtime::block_on(
        liquid::build_detail::materialise_tree(fixture.source, *root, destination));
    if (folds_case) {
        ASSERT_FALSE(materialised);
        EXPECT_EQ(materialised.error().code, liquid::errc::already_exists);
        return;
    }
    ASSERT_TRUE(materialised) << materialised.error().message;
    EXPECT_EQ(read_text(destination / "README"), "upper");
    EXPECT_EQ(read_text(destination / "Readme"), "mixed");
}

TEST(BuildMaterialise, ordinary_trees_still_materialise) {
    materialise_fixture fixture;
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("src/main.c", bytes("int main;"))));
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write(".gitignore", bytes("build/\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(fixture.source.write("a..b/...", bytes("dots"))));
    auto root = liquid::runtime::block_on(fixture.source.root_digest());
    ASSERT_TRUE(root) << root.error().message;
    const auto destination = fixture.path("ordinary");
    auto materialised = liquid::runtime::block_on(
        liquid::build_detail::materialise_tree(fixture.source, *root, destination));
    ASSERT_TRUE(materialised) << materialised.error().message;
    EXPECT_EQ(read_text(destination / "src/main.c"), "int main;");
    EXPECT_EQ(read_text(destination / ".gitignore"), "build/\n");
    EXPECT_EQ(read_text(destination / "a..b/..."), "dots");
}

} // namespace
