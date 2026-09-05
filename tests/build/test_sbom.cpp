#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>

#include "../temporary_directory.hpp"
#include "build/cache.hpp"
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
        : path_(liquid::test::fixture_path("liquid-sbom-test-" + std::to_string(next_++))) {
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

TEST(BuildSbom, repository_pages_results_attestations_traces_and_sboms) {
    stored_build fixture;

    auto results = liquid::runtime::block_on(fixture.repository.results(
        {.id = fixture.result, .output = "build/app", .content = fixture.output}));
    ASSERT_TRUE(results) << results.error().message;
    ASSERT_TRUE(results->done);
    ASSERT_EQ(results->records.size(), 1U);
    EXPECT_EQ(liquid::to_string(results->records.front().id.value),
              liquid::to_string(fixture.result.value));

    auto first_attestation =
        liquid::runtime::block_on(fixture.repository.attestations(fixture.output, {.limit = 1}));
    ASSERT_TRUE(first_attestation) << first_attestation.error().message;
    ASSERT_EQ(first_attestation->records.size(), 1U);
    ASSERT_TRUE(first_attestation->next);
    EXPECT_FALSE(first_attestation->done);
    EXPECT_EQ(first_attestation->records.front().value.profile, "sbom/1");

    auto second_attestation = liquid::runtime::block_on(
        fixture.repository.attestations({.subject = fixture.output,
                                         .profile = "sbom/1",
                                         .page = {.cursor = first_attestation->next, .limit = 1}}));
    ASSERT_TRUE(second_attestation) << second_attestation.error().message;
    ASSERT_EQ(second_attestation->records.size(), 1U);
    EXPECT_TRUE(second_attestation->done);
    EXPECT_NE(liquid::to_string(first_attestation->records.front().id),
              liquid::to_string(second_attestation->records.front().id));

    auto sboms = liquid::runtime::block_on(fixture.repository.sboms(fixture.result, {.limit = 1}));
    ASSERT_TRUE(sboms) << sboms.error().message;
    ASSERT_EQ(sboms->records.size(), 1U);
    ASSERT_TRUE(sboms->next);
    EXPECT_TRUE(sboms->records.front().verified);
    auto remaining_sboms = liquid::runtime::block_on(
        fixture.repository.sboms(fixture.result, {.cursor = sboms->next, .limit = 1}));
    ASSERT_TRUE(remaining_sboms) << remaining_sboms.error().message;
    ASSERT_EQ(remaining_sboms->records.size(), 1U);
    EXPECT_TRUE(remaining_sboms->done);

    auto result_without_sboms = results->records.front().value;
    result_without_sboms.sboms.clear();
    auto encoded_result = liquid::objects::encode(result_without_sboms);
    ASSERT_TRUE(encoded_result) << encoded_result.error().message;
    auto stored_result =
        liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
            fixture.repository, std::move(*encoded_result)));
    ASSERT_TRUE(stored_result) << stored_result.error().message;
    auto empty_sboms =
        liquid::runtime::block_on(fixture.repository.sboms(liquid::result_id{*stored_result}));
    ASSERT_TRUE(empty_sboms) << empty_sboms.error().message;
    EXPECT_TRUE(empty_sboms->records.empty());
    EXPECT_TRUE(empty_sboms->done);

    auto first_result_page =
        liquid::runtime::block_on(fixture.repository.results({.page = {.limit = 1}}));
    ASSERT_TRUE(first_result_page) << first_result_page.error().message;
    ASSERT_EQ(first_result_page->records.size(), 1U);
    ASSERT_TRUE(first_result_page->next);
    EXPECT_FALSE(first_result_page->done);
    auto second_result_page = liquid::runtime::block_on(
        fixture.repository.results({.page = {.cursor = first_result_page->next, .limit = 1}}));
    ASSERT_TRUE(second_result_page) << second_result_page.error().message;
    ASSERT_EQ(second_result_page->records.size(), 1U);
    EXPECT_TRUE(second_result_page->done);
    EXPECT_NE(liquid::to_string(first_result_page->records.front().id.value),
              liquid::to_string(second_result_page->records.front().id.value));

    auto trace = liquid::build_trace{.wall_time = std::chrono::milliseconds{12}};
    auto encoded_trace = liquid::objects::encode(trace);
    ASSERT_TRUE(encoded_trace) << encoded_trace.error().message;
    auto trace_id = liquid::runtime::block_on(liquid::repository_detail::build_access::put_record(
        fixture.repository, std::move(*encoded_trace)));
    ASSERT_TRUE(trace_id) << trace_id.error().message;
    auto cache = liquid::build_detail::disk_cache{fixture.repository};
    ASSERT_TRUE(liquid::runtime::block_on(cache.open()));
    ASSERT_TRUE(liquid::runtime::block_on(cache.record_trace(*trace_id)));
    auto traces = liquid::runtime::block_on(fixture.repository.traces({.limit = 1}));
    ASSERT_TRUE(traces) << traces.error().message;
    ASSERT_EQ(traces->records.size(), 1U);
    EXPECT_EQ(liquid::to_string(traces->records.front().id), liquid::to_string(*trace_id));

    auto invalid_page = liquid::runtime::block_on(fixture.repository.results(
        {.page = {.cursor = liquid::record_cursor{"not-a-record"}, .limit = 1}}));
    ASSERT_FALSE(invalid_page);
    EXPECT_EQ(invalid_page.error().code, liquid::errc::invalid_argument);
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
