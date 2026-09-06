#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "formats/ostree/ostree.hpp"
#if !defined(LIQUID_SHARED_TEST_BUILD)
#include "formats/tar/writer.hpp"
#endif
#include "liquid/attestation.hpp"
#include "liquid/format.hpp"
#include "liquid/hash.hpp"
#include "liquid/projection.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"
#include "projection/projection.hpp"
#include "temporary_directory.hpp"

namespace {

auto read_bytes(const std::filesystem::path& path) -> std::vector<std::byte> {
    auto input = std::ifstream{path, std::ios::binary};
    const auto text =
        std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

auto read_text(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path, std::ios::binary};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

class projection_fixture final {
public:
    projection_fixture() {
        path_ = liquid::test::fixture_path("liquid-projection");
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
        auto created = liquid::runtime::block_on(liquid::repository::create(path_ / "repository"));
        EXPECT_TRUE(created) << created.error().message;
        repository_ = std::move(*created);
        auto imported = liquid::runtime::block_on(
            repository_.import_tree(std::filesystem::path{LIQUID_PROJECTION_FIXTURES} / "root"));
        EXPECT_TRUE(imported) << imported.error().message;
        auto root = liquid::runtime::block_on(repository_.root_digest());
        EXPECT_TRUE(root) << root.error().message;
        root_ = *root;
    }

    ~projection_fixture() {
        repository_ = {};
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    auto request(liquid::projection_target target, std::string output) const
        -> liquid::projection_request {
        return {.sources = {{root_}},
                .metadata = {.name = "liquid-fixture",
                             .version = "1.2.3",
                             .release = "4",
                             .architecture = "x86_64",
                             .description = "Liquid projection fixture",
                             .maintainer = "Liquid maintainers",
                             .licence = "Apache-2.0",
                             .homepage = "https://example.invalid/liquid",
                             .declared_dependencies = {"declared-extra"},
                             .provides = {"liquid-fixture-command"},
                             .conflicts = {"liquid-fixture-old"},
                             .replaces = {"liquid-fixture-preview"}},
                .target = target,
                .options = {.output_directory = path_ / std::move(output),
                            .source_date_epoch = 1'700'000'000,
                            .conffiles = {"etc/liquid-projection.conf"},
                            .oci_entrypoint = {"/usr/bin/liquid-fixture"},
                            .oci_command = {"serve"}}};
    }

    auto project(liquid::projection_request request) -> liquid::projection_report {
        auto report = liquid::runtime::block_on(liquid::project(repository_, std::move(request)));
        EXPECT_TRUE(report) << report.error().message << ": " << report.error().context;
        return std::move(*report);
    }

    auto repository() -> liquid::repository& {
        return repository_;
    }

    auto root() const -> liquid::digest {
        return root_;
    }

    auto incompatible_result() -> liquid::result_id {
        auto statement =
            liquid::attestation{.subject = root_,
                                .profile = "abi_compatibility/1",
                                .at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}},
                                .subject_kind = liquid::attestation_subject_kind::library,
                                .attributes = {{"compatible_with", "sha256:previous"},
                                               {"compatibility", "incompatible"}}};
        auto signed_statement = liquid::runtime::block_on(
            liquid::repository_detail::build_access::sign_attestation(repository_, statement));
        EXPECT_TRUE(signed_statement) << signed_statement.error().message;
        auto stored_statement =
            liquid::runtime::block_on(repository_.store_attestation(std::move(statement)));
        EXPECT_TRUE(stored_statement) << stored_statement.error().message;
        const auto action =
            liquid::objects::action_record{.input = {.command = {"install", "liquid-fixture"},
                                                     .outputs = {"bin/liquid-fixture"},
                                                     .inputs = root_,
                                                     .toolchain = root_,
                                                     .platform = {"linux", "x86_64"}},
                                           .sandbox = root_};
        auto value = liquid::action_result{
            .exit = 0,
            .stdout_output = root_,
            .stderr_output = root_,
            .worker = "fixture/worker",
            .observed_at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}},
            .abi_compatibility = {*stored_statement},
            .output_tree = root_};
        value.finished_at = value.observed_at;
        auto stored =
            liquid::runtime::block_on(liquid::build_detail::testing::store_result_with_provenance(
                repository_, action, value));
        EXPECT_TRUE(stored) << stored.error().message;
        value.abi_compatibility = {*stored_statement};
        auto signed_result = liquid::runtime::block_on(
            liquid::repository_detail::build_access::sign_result(repository_, value));
        EXPECT_TRUE(signed_result) << signed_result.error().message;
        auto encoded = liquid::objects::encode(value);
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto stored_with_verdict = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository_, std::move(*encoded)));
        EXPECT_TRUE(stored_with_verdict) << stored_with_verdict.error().message;
        return liquid::result_id{*stored_with_verdict};
    }

    auto result_with_evidence() -> liquid::result_id {
        auto root_tree = liquid::runtime::block_on(repository_.tree(root_));
        EXPECT_TRUE(root_tree) << root_tree.error().message;
        const auto bin = std::ranges::find(root_tree->entries, "bin", &liquid::tree_entry::name);
        EXPECT_NE(bin, root_tree->entries.end());
        auto bin_tree = liquid::runtime::block_on(repository_.tree(bin->ref));
        EXPECT_TRUE(bin_tree) << bin_tree.error().message;
        const auto executable =
            std::ranges::find(bin_tree->entries, "liquid-fixture", &liquid::tree_entry::name);
        EXPECT_NE(executable, bin_tree->entries.end());
        const auto action =
            liquid::objects::action_record{.input = {.command = {"install", "liquid-fixture"},
                                                     .outputs = {"bin/liquid-fixture"},
                                                     .inputs = root_,
                                                     .toolchain = root_,
                                                     .platform = {"linux", "x86_64"}},
                                           .sandbox = root_};
        auto value = liquid::action_result{
            .exit = 0,
            .outputs = {{"bin/liquid-fixture", 0100755, executable->ref}},
            .stdout_output = executable->ref,
            .stderr_output = executable->ref,
            .worker = "fixture/worker",
            .observed_at = std::chrono::sys_seconds{std::chrono::seconds{1'700'000'000}},
            .output_tree = root_};
        value.finished_at = value.observed_at;
        auto stored =
            liquid::runtime::block_on(liquid::build_detail::testing::store_result_with_provenance(
                repository_, action, value));
        EXPECT_TRUE(stored) << stored.error().message;
        EXPECT_TRUE(value.provenance);
        EXPECT_EQ(value.sboms.size(), 2U);
        return *stored;
    }

private:
    std::filesystem::path path_;
    liquid::repository repository_;
    liquid::digest root_;
};

TEST(Projection, package_writers_round_trip_and_are_byte_identical) {
    auto fixture = projection_fixture{};
    const auto targets = std::array{liquid::projection_target::deb,
                                    liquid::projection_target::rpm,
                                    liquid::projection_target::apk};
    const auto expected = std::array{
        std::string_view{"sha256:8db7c54526a29c616dd8a2135b88bd6c52567bafb7bf2e6f875678571f9245ae"},
        std::string_view{"sha256:1561b30a7e8a8a17db973c677a89a198050b238147cae667eb4d584ad2d0b842"},
        std::string_view{
            "sha256:c163266a9018406de4096510c65675ee2d9c27083577d19ad4967bd479be3c0d"}};
    for (auto index = std::size_t{}; index < targets.size(); ++index) {
        const auto target = targets[index];
        auto first_request = fixture.request(target, std::string{liquid::to_string(target)});
        auto second_request =
            fixture.request(target, std::string{liquid::to_string(target)} + "-again");
        first_request.metadata.declared_dependencies.clear();
        second_request.metadata.declared_dependencies.clear();
        auto first = fixture.project(std::move(first_request));
        auto second = fixture.project(std::move(second_request));
        ASSERT_EQ(first.artifacts.size(), 1U);
        ASSERT_EQ(second.artifacts.size(), 1U);
        EXPECT_EQ(liquid::to_string(first.artifacts.front().digest),
                  liquid::to_string(second.artifacts.front().digest));
        EXPECT_EQ(liquid::to_string(first.artifacts.front().digest), expected[index]);
        EXPECT_EQ(read_bytes(first.artifacts.front().path),
                  read_bytes(second.artifacts.front().path));
        auto bytes = read_bytes(first.artifacts.front().path);
        auto recognised = liquid::format_registry::global().recognise(
            bytes, first.artifacts.front().path.filename().string());
        ASSERT_TRUE(recognised) << recognised.error().message;
        auto unpacked = liquid::format_registry::global().unpack(
            bytes, first.artifacts.front().path.filename().string());
        ASSERT_TRUE(unpacked) << unpacked.error().message;
        EXPECT_EQ(unpacked->codec, liquid::to_string(target));
        const auto expected_path = target == liquid::projection_target::rpm
                                       ? "data/./usr/bin/liquid-fixture"
                                       : "data/usr/bin/liquid-fixture";
        EXPECT_TRUE(
            std::ranges::contains(unpacked->entries, expected_path, &liquid::format_entry::path));
    }
}

TEST(Projection, package_reader_limits_are_enforced) {
    auto fixture = projection_fixture{};
    auto report = fixture.project(fixture.request(liquid::projection_target::deb, "limited-deb"));
    const auto bytes = read_bytes(report.artifacts.front().path);
    auto archive =
        liquid::format_registry::global().unpack(bytes,
                                                 report.artifacts.front().path.filename().string(),
                                                 liquid::format_limits{.byte_budget = 1});
    ASSERT_FALSE(archive);
    EXPECT_EQ(archive.error().code, liquid::errc::corrupt);
}

TEST(Projection, dependency_capabilities_follow_each_package_ecosystem) {
    const auto prepared = liquid::projection_detail::prepared_projection{
        .metadata = {.architecture = "x86_64", .declared_dependencies = {"declared-extra"}},
        .needed = {"libc.so.6", "libfoo.so.1"},
        .sonames = {"libfixture.so.1"}};
    EXPECT_EQ(liquid::projection_detail::dependencies(prepared, liquid::projection_target::deb),
              (std::vector<std::string>{"declared-extra", "libc6", "libfoo.so.1"}));
    EXPECT_EQ(
        liquid::projection_detail::dependencies(prepared, liquid::projection_target::rpm),
        (std::vector<std::string>{"declared-extra", "libc.so.6()(64bit)", "libfoo.so.1()(64bit)"}));
    EXPECT_EQ(liquid::projection_detail::dependencies(prepared, liquid::projection_target::apk),
              (std::vector<std::string>{"declared-extra", "so:libc.so.6", "so:libfoo.so.1"}));
    EXPECT_EQ(liquid::projection_detail::provides(prepared, liquid::projection_target::rpm),
              (std::vector<std::string>{"libfixture.so.1()(64bit)"}));
}

TEST(Projection, build_trace_distance_selects_stability_layers_deterministically) {
    auto digest = [](std::uint8_t value) {
        auto output = liquid::digest{liquid::algorithm::blake3, {}};
        output.bytes.front() = std::byte{value};
        return output;
    };
    const auto low = digest(1);
    const auto middle = digest(2);
    const auto high = digest(3);
    const auto trace =
        liquid::build_trace{.actions = {{.action = low, .outputs = {"./lib/runtime.so"}},
                                        {.action = middle, .outputs = {"lib/project.so"}},
                                        {.action = high, .outputs = {"/bin/application"}}},
                            .edges = {{low, middle}, {middle, high}}};
    const auto first = liquid::projection_detail::trace_output_layers(trace, high);
    const auto second = liquid::projection_detail::trace_output_layers(trace, high);
    EXPECT_EQ(first, second);
    EXPECT_EQ(first.at("lib/runtime.so"), 0);
    EXPECT_EQ(first.at("lib/project.so"), 1);
    EXPECT_EQ(first.at("bin/application"), 2);
}

TEST(Projection, deb_compatibility_mode_uses_deterministic_gzip) {
    auto fixture = projection_fixture{};
    auto request = fixture.request(liquid::projection_target::deb, "deb-gzip");
    request.options.compat_gzip = true;
    auto report = fixture.project(request);
    EXPECT_TRUE(std::ranges::contains(
        report.dependencies, "declared-extra", &liquid::projection_dependency::value));
    const auto bytes = read_bytes(report.artifacts.front().path);
    auto archive = liquid::format_registry::global().unpack(
        bytes, report.artifacts.front().path.filename().string());
    ASSERT_TRUE(archive) << archive.error().message;
    const auto metadata = std::ranges::find(
        archive->entries, std::string_view{".metadata"}, &liquid::format_entry::path);
    ASSERT_NE(metadata, archive->entries.end());
    const auto control =
        std::string{reinterpret_cast<const char*>(metadata->bytes.data()), metadata->bytes.size()};
    EXPECT_NE(control.find("Package: liquid-fixture"), std::string::npos);
    EXPECT_NE(control.find("Depends: declared-extra"), std::string::npos);
}

TEST(Projection, incompatible_abi_requires_an_explicit_override) {
    auto fixture = projection_fixture{};
    const auto result = fixture.incompatible_result();
    auto request = fixture.request(liquid::projection_target::deb, "abi-refused");
    request.sources = {{result}};
    request.metadata.architecture.clear();
    auto refused = liquid::runtime::block_on(liquid::project(fixture.repository(), request));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::conflict);

    request.options.output_directory =
        request.options.output_directory.parent_path() / "abi-allowed";
    request.options.allow_abi_break = true;
    auto allowed = fixture.project(std::move(request));
    EXPECT_TRUE(allowed.abi_break_allowed);
}

TEST(Projection, package_metadata_rejects_control_characters) {
    auto fixture = projection_fixture{};
    auto request = fixture.request(liquid::projection_target::deb, "invalid-metadata");
    request.metadata.description = "valid\nDepends: injected";
    auto report = liquid::runtime::block_on(liquid::project(fixture.repository(), request));
    ASSERT_FALSE(report);
    EXPECT_EQ(report.error().code, liquid::errc::invalid_argument);
}

TEST(Projection, oci_layout_is_deterministic_and_lists_attestation_referrers) {
    auto fixture = projection_fixture{};
    auto first = fixture.project(fixture.request(liquid::projection_target::oci_image, "oci"));
    auto second =
        fixture.project(fixture.request(liquid::projection_target::oci_image, "oci-again"));
    ASSERT_EQ(first.artifacts.size(), 1U);
    ASSERT_EQ(second.artifacts.size(), 1U);
    EXPECT_EQ(liquid::to_string(first.artifacts.front().digest),
              liquid::to_string(second.artifacts.front().digest));
    EXPECT_EQ(liquid::to_string(first.artifacts.front().digest),
              "sha256:7c7a30f0fbf33d4ce4864c40ea5312489cff5ff7fe8a1c02a5fda385d2779e42");
    const auto index = read_text(first.artifacts.front().path / "index.json");
    EXPECT_NE(index.find("application/vnd.dsse.envelope.v1+json"), std::string::npos);
    EXPECT_NE(index.find("sha256-"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(first.artifacts.front().path / "oci-layout"));
    ASSERT_EQ(first.attestations.size(), 1U);
    auto statements = liquid::runtime::block_on(
        fixture.repository().attestations({.id = first.attestations.back()}));
    ASSERT_TRUE(statements) << statements.error().message;
    ASSERT_EQ(statements->records.size(), 1U);
    EXPECT_EQ(statements->records.front().value.profile, "projection/1");
    auto verified = liquid::verify_attestation(statements->records.front().value);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;
}

TEST(Projection, oci_layout_attaches_source_provenance_and_sboms) {
    auto fixture = projection_fixture{};
    const auto result = fixture.result_with_evidence();
    auto request = fixture.request(liquid::projection_target::oci_image, "oci-evidence");
    request.sources = {{result}};
    request.metadata.architecture.clear();
    auto report = fixture.project(std::move(request));
    EXPECT_EQ(report.attestations.size(), 4U);
    const auto index = read_text(report.artifacts.front().path / "index.json");
    auto offset = std::size_t{};
    auto count = std::size_t{};
    while ((offset = index.find("application/vnd.dsse.envelope.v1+json", offset)) !=
           std::string::npos) {
        ++count;
        ++offset;
    }
    EXPECT_EQ(count, 4U);
}

TEST(Projection, ostree_commit_matches_the_glib_encoded_fixture) {
    auto fixture = projection_fixture{};
    auto first_request = fixture.request(liquid::projection_target::ostree_commit, "ostree");
    auto second_request = fixture.request(liquid::projection_target::ostree_commit, "ostree-again");
    auto bare_request = fixture.request(liquid::projection_target::ostree_commit, "ostree-bare");
    first_request.options.ostree_ref = "liquid/os/x86_64";
    second_request.options.ostree_ref = first_request.options.ostree_ref;
    bare_request.options.ostree_ref = first_request.options.ostree_ref;
    bare_request.options.ostree_mode = liquid::ostree_repository_mode::bare_user;
    auto first = fixture.project(std::move(first_request));
    auto second = fixture.project(std::move(second_request));
    auto bare = fixture.project(std::move(bare_request));
    ASSERT_TRUE(first.ostree_commit);
    ASSERT_TRUE(second.ostree_commit);
    EXPECT_EQ(liquid::to_string(*first.ostree_commit), liquid::to_string(*second.ostree_commit));
    EXPECT_EQ(liquid::to_string(*first.ostree_commit),
              "sha256:01b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4");
    EXPECT_EQ(read_bytes(first.artifacts.front().path / "summary"),
              read_bytes(second.artifacts.front().path / "summary"));
    auto summary = liquid::hash(liquid::algorithm::sha256,
                                read_bytes(first.artifacts.front().path / "summary"));
    ASSERT_TRUE(summary) << summary.error().message;
    EXPECT_EQ(liquid::to_string(*summary),
              "sha256:66ed85850cfa0536b7af1ac6ec16de7bbf1505534b575323545557e08fd8f94c");
    EXPECT_EQ(read_text(first.artifacts.front().path / "refs/heads/liquid/os/x86_64"),
              "01b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4\n");
    EXPECT_EQ(read_text(first.artifacts.front().path / "config"),
              "[core]\nrepo_version=1\nmode=archive-z2\n");
    EXPECT_EQ(read_text(bare.artifacts.front().path / "config"),
              "[core]\nrepo_version=1\nmode=bare-user\n");
    EXPECT_TRUE(std::filesystem::exists(
        bare.artifacts.front().path / "objects/6d" /
        "e0e97942c5e5d1cc7fd4122c932d68575eb6bf5567f067a4a81b3ae85c02aa.file"));
    const auto commit_path =
        first.artifacts.front().path / "objects/01" /
        "b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4.commit";
    EXPECT_TRUE(std::filesystem::exists(commit_path));
    auto json = liquid::projection_report_json(first);
    ASSERT_TRUE(json) << json.error().message;
    EXPECT_NE(json->find("01b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4"),
              std::string::npos);
    auto statements = liquid::runtime::block_on(
        fixture.repository().attestations({.id = first.attestations.back()}));
    ASSERT_TRUE(statements) << statements.error().message;
    ASSERT_EQ(statements->records.size(), 1U);
    const auto& attributes = statements->records.front().value.attributes;
    EXPECT_TRUE(
        std::ranges::contains(attributes, "base_tree", &liquid::attestation_attribute::name));
}

TEST(Projection, ostree_overlays_are_mounted_at_declared_paths) {
    auto fixture = projection_fixture{};
    auto request = fixture.request(liquid::projection_target::ostree_commit, "ostree-overlay");
    request.sources.push_back({fixture.root(), "bin/liquid-fixture"});
    request.options.ostree_ref = "liquid/os/overlay";
    const auto report = fixture.project(std::move(request));
    ASSERT_TRUE(report.ostree_commit);
    EXPECT_EQ(liquid::to_string(*report.ostree_commit),
              "sha256:583b710f4ec7f2c1b436b996bf0c9fbad1e457ac739b69fae2fdb9e67078d2a7");
}

TEST(Projection, ostree_writer_preserves_symbolic_links) {
    const auto target = std::string(140, 'a');
    const auto bytes =
        std::vector<std::byte>{reinterpret_cast<const std::byte*>(target.data()),
                               reinterpret_cast<const std::byte*>(target.data() + target.size())};
    const auto files = std::array{liquid::formats::package::file{"link", bytes, 0120777U}};
    auto built = liquid::formats::ostree::build(
        files, {}, {{"link", "base"}}, "liquid/os/link", 1'700'000'000);
    ASSERT_TRUE(built) << built.error().message;
    ASSERT_EQ(built->files.size(), 1U);
    EXPECT_EQ(liquid::to_string(built->files.front().checksum),
              "sha256:38e00879da8c0e5c496e5c938846a5509d8b19d05c24ee2761cd86e5a733f95c");

    const auto root = liquid::test::fixture_path("ostree-symlink");
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    ASSERT_FALSE(code) << code.message();
    auto archive = liquid::formats::ostree::write(
        *built, root / "archive", liquid::ostree_repository_mode::archive_z2);
    ASSERT_TRUE(archive) << archive.error().message;
    auto bare = liquid::formats::ostree::write(
        *built, root / "bare", liquid::ostree_repository_mode::bare_user);
    ASSERT_TRUE(bare) << bare.error().message;
    const auto object = std::filesystem::path{"objects/38"} /
                        "e00879da8c0e5c496e5c938846a5509d8b19d05c24ee2761cd86e5a733f95c";
    EXPECT_TRUE(std::filesystem::exists(root / "archive" / (object.string() + ".filez")));
    auto bare_bytes = read_bytes(root / "bare" / (object.string() + ".file"));
    ASSERT_EQ(bare_bytes.size(), target.size() + 1);
    EXPECT_EQ(bare_bytes.back(), std::byte{});

#if !defined(LIQUID_SHARED_TEST_BUILD)
    const auto entries = std::array{liquid::formats::tar::entry{
        .path = "link", .mode = 0777, .link_path = target, .symbolic_link = true}};
    auto tar = liquid::formats::tar::write(entries, 1'700'000'000);
    ASSERT_TRUE(tar) << tar.error().message;
    auto unpacked = liquid::format_registry::global().unpack(*tar, "links.tar");
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    const auto link = std::ranges::find(unpacked->entries, "link", &liquid::format_entry::path);
    ASSERT_NE(link, unpacked->entries.end());
    const auto attributes = std::string{reinterpret_cast<const char*>(link->attributes.data()),
                                        link->attributes.size()};
    EXPECT_NE(attributes.find(target), std::string::npos);
#endif
}

TEST(Projection, ostree_gvariant_offsets_match_glib_past_one_byte) {
    const auto path = liquid::test::fixture_path("ostree-wide-directory");
    auto code = std::error_code{};
    std::filesystem::remove_all(path, code);
    ASSERT_FALSE(code) << code.message();
    ASSERT_TRUE(std::filesystem::create_directories(path / "source"));
    for (auto index = 0; index < 300; ++index) {
        auto file = std::ofstream{path / "source" / std::format("file{:03}", index)};
        ASSERT_TRUE(file);
    }
    auto repository = liquid::runtime::block_on(liquid::repository::create(path / "repository"));
    ASSERT_TRUE(repository) << repository.error().message;
    auto imported = liquid::runtime::block_on(repository->import_tree(path / "source"));
    ASSERT_TRUE(imported) << imported.error().message;
    auto root = liquid::runtime::block_on(repository->root_digest());
    ASSERT_TRUE(root) << root.error().message;
    auto request =
        liquid::projection_request{.sources = {{{*root}}},
                                   .metadata = {.name = "wide-directory", .version = "1"},
                                   .target = liquid::projection_target::ostree_commit,
                                   .options = {.output_directory = path / "output",
                                               .source_date_epoch = 1'700'000'000,
                                               .ostree_ref = "liquid/os/wide-directory"}};
    auto report = liquid::runtime::block_on(liquid::project(*repository, std::move(request)));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_TRUE(report->ostree_commit);
    EXPECT_EQ(liquid::to_string(*report->ostree_commit),
              "sha256:26a345f7c249d50ac8ad2e14365934b588e663e666748fe2b01d3f5941e26d56");
}

TEST(Projection, bootc_layout_is_deterministic_and_carries_the_ostree_commit) {
    auto fixture = projection_fixture{};
    auto first_request = fixture.request(liquid::projection_target::bootc_image, "bootc");
    auto second_request = fixture.request(liquid::projection_target::bootc_image, "bootc-again");
    first_request.options.ostree_ref = "liquid/os/x86_64";
    second_request.options.ostree_ref = first_request.options.ostree_ref;
    auto first = fixture.project(std::move(first_request));
    auto second = fixture.project(std::move(second_request));
    ASSERT_TRUE(first.ostree_commit);
    ASSERT_TRUE(second.ostree_commit);
    EXPECT_EQ(liquid::to_string(*first.ostree_commit),
              "sha256:01b3c73af4e373360c6fbb04258c53ed526ba89056c321e4f144a07edbec04b4");
    EXPECT_EQ(liquid::to_string(first.artifacts.front().digest),
              liquid::to_string(second.artifacts.front().digest));
    EXPECT_EQ(liquid::to_string(first.artifacts.front().digest),
              "sha256:e21597809fa09ce8888f97c3391e1f1328a6d4d6cd1bbee8be488bd139900728");
    const auto manifest = read_text(
        first.artifacts.front().path / "blobs/sha256" /
        liquid::to_string(first.artifacts.front().digest).substr(std::string{"sha256:"}.size()));
    EXPECT_NE(manifest.find("containers.bootc"), std::string::npos);
    EXPECT_NE(manifest.find("ostree.bootable"), std::string::npos);
    EXPECT_NE(manifest.find("ostree.commit"), std::string::npos);
    EXPECT_NE(manifest.find("ostree.final-diffid"), std::string::npos);
    EXPECT_NE(manifest.find("ostree.encapsulated"), std::string::npos);
    EXPECT_NE(manifest.find("ostree.components"), std::string::npos);
    const auto index = read_text(first.artifacts.front().path / "index.json");
    EXPECT_NE(index.find("application/vnd.dsse.envelope.v1+json"), std::string::npos);
}

TEST(Projection, helm_chart_uses_digest_references_and_has_an_oci_artifact) {
    auto fixture = projection_fixture{};
    auto request = fixture.request(liquid::projection_target::helm_chart, "helm");
    auto image = liquid::digest{liquid::algorithm::sha256, {}};
    image.bytes.front() = std::byte{0x42};
    request.services = {{.name = "api",
                         .image_repository = "registry.example.invalid/liquid/api",
                         .image = image,
                         .ports = {8080},
                         .env = {{"MODE", "fixture"}},
                         .volumes = {"/data"}}};
    auto report = fixture.project(request);
    ASSERT_EQ(report.artifacts.size(), 2U);
    EXPECT_EQ(liquid::to_string(report.artifacts.front().digest),
              "sha256:40008291c81e2957f681684ec5cbe02f30018953eb368b3a5f7f6f408227a4ea");
    EXPECT_EQ(liquid::to_string(report.artifacts.back().digest),
              "sha256:2e7396003fbed713df6895abcb10294b4e6c645dcef8b664cdb67dfa761d4315");
    const auto chart = report.artifacts.front().path;
    const auto values = read_text(chart / "values.yaml");
    EXPECT_NE(values.find(liquid::to_string(image)), std::string::npos);
    EXPECT_EQ(values.find(":latest"), std::string::npos);
    EXPECT_TRUE(std::filesystem::exists(chart / "templates" / "deployment.yaml"));
    EXPECT_TRUE(std::filesystem::exists(report.artifacts.back().path / "index.json"));
#if defined(LIQUID_HELM_EXECUTABLE)
    const auto command = std::string{LIQUID_HELM_EXECUTABLE} + " lint " + chart.string();
    EXPECT_EQ(std::system(command.c_str()), 0);
#endif
}

TEST(Projection, emit_peer_artifacts_when_requested) {
    const auto* output = std::getenv("LIQUID_PROJECTION_PEER_OUT");
    if (output == nullptr) {
        GTEST_SKIP() << "LIQUID_PROJECTION_PEER_OUT is not set";
    }
    auto fixture = projection_fixture{};
    const auto root = std::filesystem::path{output};
    auto code = std::error_code{};
    std::filesystem::create_directories(root, code);
    ASSERT_FALSE(code) << code.message();
    for (const auto target : {liquid::projection_target::deb,
                              liquid::projection_target::rpm,
                              liquid::projection_target::apk}) {
        auto request = fixture.request(target, (root / liquid::to_string(target)).string());
        request.metadata.declared_dependencies.clear();
        auto report = fixture.project(std::move(request));
        std::cout << liquid::to_string(target) << '=' << report.artifacts.front().path << ' '
                  << liquid::to_string(report.artifacts.front().digest) << '\n';
    }
    auto image_request =
        fixture.request(liquid::projection_target::oci_image, (root / "oci").string());
    image_request.metadata.declared_dependencies.clear();
    auto image = fixture.project(std::move(image_request));
    std::cout << "oci=" << image.artifacts.front().path << ' '
              << liquid::to_string(image.artifacts.front().digest) << '\n';

    auto ostree_request =
        fixture.request(liquid::projection_target::ostree_commit, (root / "ostree").string());
    ostree_request.options.ostree_ref = "liquid/os/x86_64";
    auto ostree = fixture.project(std::move(ostree_request));
    std::cout << "ostree=" << ostree.artifacts.front().path << ' '
              << liquid::to_string(*ostree.ostree_commit) << '\n';

    auto bootc_request =
        fixture.request(liquid::projection_target::bootc_image, (root / "bootc").string());
    bootc_request.options.ostree_ref = "liquid/os/x86_64";
    auto bootc = fixture.project(std::move(bootc_request));
    std::cout << "bootc=" << bootc.artifacts.front().path << ' '
              << liquid::to_string(*bootc.ostree_commit) << ' '
              << liquid::to_string(bootc.artifacts.front().digest) << '\n';

    auto chart_request =
        fixture.request(liquid::projection_target::helm_chart, (root / "helm").string());
    chart_request.metadata.declared_dependencies.clear();
    chart_request.services = {{.name = "fixture",
                               .image_repository = "liquid-fixture",
                               .image = image.artifacts.front().digest}};
    auto chart = fixture.project(std::move(chart_request));
    std::cout << "helm=" << chart.artifacts.front().path << ' '
              << liquid::to_string(chart.artifacts.front().digest) << ' '
              << liquid::to_string(chart.artifacts.back().digest) << '\n';
}

} // namespace
