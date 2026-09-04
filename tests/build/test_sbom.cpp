#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>

#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "formats/json.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

class temporary_repository {
public:
    temporary_repository()
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-sbom-test-" + std::to_string(next_++))) {
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

auto example_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

auto stored_tree(liquid::repository& repository, liquid::tree value) -> liquid::digest {
    auto encoded = liquid::objects::encode(value);
    EXPECT_TRUE(encoded) << encoded.error().message;
    auto stored = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(repository, std::move(*encoded)));
    EXPECT_TRUE(stored) << stored.error().message;
    return *stored;
}

auto field(const liquid::formats::json::value& value, std::string_view name)
    -> const liquid::formats::json::value* {
    const auto* object = std::get_if<liquid::formats::json::value::object>(&value.data);
    if (!object) {
        return nullptr;
    }
    const auto found = std::ranges::find(
        *object, name, [](const auto& entry) { return std::string_view{entry.first}; });
    return found == object->end() ? nullptr : &found->second;
}

auto text_field(const liquid::formats::json::value& value, std::string_view name)
    -> const std::string* {
    const auto* item = field(value, name);
    return item ? std::get_if<std::string>(&item->data) : nullptr;
}

auto count(std::string_view input, std::string_view needle) -> std::size_t {
    auto output = std::size_t{};
    for (auto offset = input.find(needle); offset != std::string_view::npos;
         offset = input.find(needle, offset + needle.size())) {
        ++output;
    }
    return output;
}

struct stored_build {
    temporary_repository location;
    liquid::repository repository;
    liquid::result_id result;
    liquid::digest input_a{example_digest(1)};
    liquid::digest input_b{example_digest(2)};
    liquid::digest compiler{example_digest(3)};
    liquid::digest linker{example_digest(4)};
    liquid::digest output{example_digest(5)};
    liquid::digest pinned_change{example_digest(6)};

    stored_build() {
        auto opened = liquid::runtime::block_on(liquid::repository::create(location.path()));
        EXPECT_TRUE(opened) << opened.error().message;
        repository = std::move(*opened);

        auto repository_id = liquid::node_id{};
        repository_id.front() = std::byte{0x2a};
        const auto inputs = stored_tree(
            repository,
            {.entries = {{"left.c", 0644, liquid::node_kind::blob, input_a, {}, input_a},
                         {"right.c", 0644, liquid::node_kind::blob, input_b, {}, input_b},
                         {"vendor",
                          0755,
                          liquid::node_kind::repo,
                          pinned_change,
                          repository_id,
                          pinned_change}}});
        const auto tool_directory = stored_tree(
            repository, {.entries = {{"ld", 0755, liquid::node_kind::blob, linker, {}, linker}}});
        const auto toolchain = stored_tree(
            repository,
            {.entries = {
                 {"cc", 0755, liquid::node_kind::blob, compiler, {}, compiler},
                 {"bin", 0755, liquid::node_kind::tree, tool_directory, {}, tool_directory}}});
        const auto action = liquid::objects::action_record{
            .input = {.command = {"cc", "left.c", "right.c", "-o", "app"},
                      .cwd = "src",
                      .env = {{"LANG", "C.UTF-8"}},
                      .outputs = {"build/app"},
                      .inputs = inputs,
                      .toolchain = toolchain,
                      .platform = {"test", "test"}},
            .sandbox = example_digest(7)};
        auto value = liquid::action_result{.exit = 0,
                                           .outputs = {{"build/app", 0755, output}},
                                           .stdout_output = example_digest(8),
                                           .stderr_output = example_digest(9),
                                           .manifest = {{"left.c", input_a}, {"right.c", input_b}},
                                           .worker = "test/test",
                                           .observed_at =
                                               std::chrono::sys_seconds{std::chrono::seconds{100}}};
        value.started_at = std::chrono::sys_seconds{std::chrono::seconds{101}};
        value.finished_at = std::chrono::sys_seconds{std::chrono::seconds{102}};
        auto stored = liquid::runtime::block_on(
            liquid::build_detail::testing::store_result_with_provenance(repository, action, value));
        EXPECT_TRUE(stored) << stored.error().message;
        result = *stored;
    }
};

TEST(BuildSbom, derives_spdx_and_cyclonedx_from_the_same_graph) {
    stored_build fixture;
    auto spdx = liquid::runtime::block_on(liquid::build::generate_sbom(
        fixture.repository, fixture.result, "build/app", liquid::sbom_format::spdx));
    ASSERT_TRUE(spdx) << spdx.error().message;
    ASSERT_TRUE(spdx->verified) << spdx->reason;
    EXPECT_TRUE(spdx->derived);
    EXPECT_FALSE(spdx->inferred);
    EXPECT_EQ(liquid::to_string(spdx->subject), liquid::to_string(fixture.output));
    ASSERT_EQ(spdx->components.size(), 6U);
    EXPECT_TRUE(std::ranges::all_of(spdx->components,
                                    [](const auto& component) { return !component.inferred; }));
    for (const auto& expected :
         {fixture.input_a, fixture.input_b, fixture.compiler, fixture.linker}) {
        EXPECT_TRUE(std::ranges::any_of(spdx->components, [&](const auto& component) {
            return component.content &&
                   liquid::to_string(*component.content) == liquid::to_string(expected);
        }));
        EXPECT_NE(spdx->document.find(liquid::to_string(expected)), std::string::npos);
    }
    const auto repository_component =
        std::ranges::find(spdx->components, "repository", &liquid::sbom_component::kind);
    ASSERT_NE(repository_component, spdx->components.end());
    EXPECT_EQ(repository_component->repository_id, "2a000000000000000000000000000000");
    ASSERT_TRUE(repository_component->pinned_change);
    EXPECT_EQ(liquid::to_string(*repository_component->pinned_change),
              liquid::to_string(fixture.pinned_change));

    auto spdx_json = liquid::formats::json::parse(spdx->document);
    ASSERT_TRUE(spdx_json) << spdx_json.error().message;
    ASSERT_NE(text_field(*spdx_json, "spdxVersion"), nullptr);
    EXPECT_EQ(*text_field(*spdx_json, "spdxVersion"), "SPDX-2.3");
    EXPECT_NE(field(*spdx_json, "creationInfo"), nullptr);
    EXPECT_NE(field(*spdx_json, "packages"), nullptr);
    EXPECT_NE(field(*spdx_json, "relationships"), nullptr);
    EXPECT_NE(field(*spdx_json, "annotations"), nullptr);

    auto cyclonedx = liquid::runtime::block_on(liquid::build::generate_sbom(
        fixture.repository, fixture.result, "build/app", liquid::sbom_format::cyclonedx));
    ASSERT_TRUE(cyclonedx) << cyclonedx.error().message;
    ASSERT_TRUE(cyclonedx->verified) << cyclonedx->reason;
    EXPECT_EQ(cyclonedx->components.size(), spdx->components.size());
    EXPECT_EQ(cyclonedx->relationships.size(), spdx->relationships.size());
    auto cyclonedx_json = liquid::formats::json::parse(cyclonedx->document);
    ASSERT_TRUE(cyclonedx_json) << cyclonedx_json.error().message;
    ASSERT_NE(text_field(*cyclonedx_json, "bomFormat"), nullptr);
    EXPECT_EQ(*text_field(*cyclonedx_json, "bomFormat"), "CycloneDX");
    ASSERT_NE(text_field(*cyclonedx_json, "specVersion"), nullptr);
    EXPECT_EQ(*text_field(*cyclonedx_json, "specVersion"), "1.5");
    EXPECT_NE(field(*cyclonedx_json, "metadata"), nullptr);
    EXPECT_NE(field(*cyclonedx_json, "components"), nullptr);
    EXPECT_NE(field(*cyclonedx_json, "dependencies"), nullptr);
}

TEST(BuildSbom, foreign_system_binary_is_inferred_everywhere_and_never_derived) {
#if defined(_WIN32)
    const auto* system_root = std::getenv("SystemRoot");
    ASSERT_NE(system_root, nullptr);
    const auto binary = std::filesystem::path{system_root} / "System32/cmd.exe";
#else
    const auto binary = std::filesystem::path{"/bin/ls"};
#endif
    for (const auto format : {liquid::sbom_format::spdx, liquid::sbom_format::cyclonedx}) {
        auto report = liquid::runtime::block_on(liquid::build::infer_sbom(binary, format));
        ASSERT_TRUE(report) << report.error().message;
        ASSERT_TRUE(report->verified) << report->reason;
        EXPECT_FALSE(report->derived);
        EXPECT_TRUE(report->inferred);
        ASSERT_FALSE(report->components.empty());
        EXPECT_TRUE(std::ranges::all_of(report->components,
                                        [](const auto& component) { return component.inferred; }));
        const auto verified = liquid::verify_attestation(report->statement);
        ASSERT_TRUE(verified) << verified.error().message;
        EXPECT_TRUE(verified->verified) << verified->reason;
        if (format == liquid::sbom_format::spdx) {
            EXPECT_EQ(count(report->document, "liquid:inferred=true"), report->components.size());
        } else {
            EXPECT_EQ(count(report->document, "liquid:inferred"), report->components.size());
        }
    }
}

} // namespace
