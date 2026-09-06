#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "build/provenance.hpp"
#include "build/repository_access.hpp"
#include "liquid/attestation.hpp"
#include "liquid/format.hpp"
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

auto install_intent_fixture() -> liquid::install_intent {
    return {
        .services = {{"liquid_agent",
                      "/usr/bin/liquid-agent",
                      "liquid_agent",
                      {"network_online"},
                      liquid::service_restart::always}},
        .users = {{"liquid_agent", true, "/var/lib/liquid-agent"}},
        .groups = {{"liquid_agent", true}},
        .directories = {{"/var/lib/liquid-agent", 0750, "liquid_agent"}},
        .symlinks = {{"/usr/bin/liquid-agent-current", "/usr/lib/liquid/agent"}},
        .caches = {liquid::install_cache::ldconfig, liquid::install_cache::systemd_daemon_reload},
        .alternatives = {{"liquid-tool", "/usr/lib/liquid/tool", 40}}};
}

auto expect_packaged_scripts(const liquid::format_tree& archive,
                             liquid::projection_target target,
                             const liquid::generated_maintainer_scripts& expected) -> void {
    auto names = std::array<std::string_view, 4>{};
    if (target == liquid::projection_target::deb) {
        names = {"preinst", "postinst", "prerm", "postrm"};
    } else if (target == liquid::projection_target::rpm) {
        names = {"%pre", "%post", "%preun", "%postun"};
    } else {
        names = {".pre-install", ".post-install", ".pre-deinstall", ".post-deinstall"};
    }
    const auto values = std::array<std::string_view, 4>{
        expected.pre_install, expected.post_install, expected.pre_remove, expected.post_remove};
    for (std::size_t index = 0; index < names.size(); ++index) {
        const auto path = ".scripts/" + std::string{names[index]};
        const auto found = std::ranges::find(archive.entries, path, &liquid::format_entry::path);
        ASSERT_NE(found, archive.entries.end()) << path;
        EXPECT_EQ(
            std::string(reinterpret_cast<const char*>(found->bytes.data()), found->bytes.size()),
            values[index]);
    }
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
                                       ? "./usr/bin/liquid-fixture"
                                       : "usr/bin/liquid-fixture";
        EXPECT_TRUE(
            std::ranges::contains(unpacked->entries, expected_path, &liquid::format_entry::path));
    }
}

TEST(Projection, one_intent_has_golden_scripts_for_all_package_formats) {
    const auto intent = install_intent_fixture();
    struct script_names {
        liquid::projection_target target;
        std::string_view directory;
        std::array<std::string_view, 4> names;
    };
    const auto cases = std::array{
        script_names{
            liquid::projection_target::deb, "deb", {"preinst", "postinst", "prerm", "postrm"}},
        script_names{liquid::projection_target::rpm, "rpm", {"pre", "post", "preun", "postun"}},
        script_names{liquid::projection_target::apk,
                     "apk",
                     {"pre-install", "post-install", "pre-deinstall", "post-deinstall"}}};
    for (const auto& item : cases) {
        auto scripts = liquid::generate_maintainer_scripts(item.target, intent);
        ASSERT_TRUE(scripts) << scripts.error().message;
        const auto root =
            std::filesystem::path{LIQUID_PROJECTION_FIXTURES} / "scripts" / item.directory;
        EXPECT_EQ(scripts->pre_install, read_text(root / item.names[0]));
        EXPECT_EQ(scripts->post_install, read_text(root / item.names[1]));
        EXPECT_EQ(scripts->pre_remove, read_text(root / item.names[2]));
        EXPECT_EQ(scripts->post_remove, read_text(root / item.names[3]));
    }
}

TEST(Projection, generated_values_cannot_change_shell_structure) {
    const auto baseline = liquid::generate_maintainer_scripts(liquid::projection_target::deb,
                                                              install_intent_fixture());
    ASSERT_TRUE(baseline) << baseline.error().message;
    const auto line_count = [](const liquid::generated_maintainer_scripts& scripts) {
        return std::ranges::count(scripts.pre_install, '\n') +
               std::ranges::count(scripts.post_install, '\n') +
               std::ranges::count(scripts.pre_remove, '\n') +
               std::ranges::count(scripts.post_remove, '\n');
    };
    const auto expected_lines = line_count(*baseline);
    for (auto index = 0; index < 100; ++index) {
        auto intent = install_intent_fixture();
        const auto name = "service_" + std::to_string(index) + "-x";
        intent.services.front().name = name;
        intent.services.front().executable = "/usr/lib/" + name;
        intent.services.front().after = {"network_" + std::to_string(index)};
        auto scripts = liquid::generate_maintainer_scripts(liquid::projection_target::deb, intent);
        ASSERT_TRUE(scripts) << scripts.error().message;
        EXPECT_EQ(line_count(*scripts), expected_lines);
    }
    for (const auto value : {"bad;name",
                             "bad$name",
                             "bad`name",
                             "bad name",
                             "bad\nname",
                             "bad'name",
                             "bad&name",
                             "bad|name",
                             "bad(name"}) {
        auto intent = install_intent_fixture();
        intent.services.front().name = value;
        auto refused = liquid::generate_maintainer_scripts(liquid::projection_target::deb, intent);
        ASSERT_FALSE(refused) << value;
        EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
    }
    for (const auto value : {"/usr/bin/tool;rm",
                             "/usr/bin/$tool",
                             "/usr/bin/../tool",
                             "relative/tool",
                             "/usr//bin/tool"}) {
        auto intent = install_intent_fixture();
        intent.services.front().executable = value;
        auto refused = liquid::generate_maintainer_scripts(liquid::projection_target::deb, intent);
        ASSERT_FALSE(refused) << value;
        EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
    }
}

TEST(Projection, generated_debian_postinst_succeeds_twice) {
    const auto workspace = liquid::test::fixture_path("liquid-projection-postinst");
    auto intent = liquid::install_intent{};
    intent.symlinks.push_back({workspace / "current", workspace / "target"});
    auto scripts = liquid::generate_maintainer_scripts(liquid::projection_target::deb, intent);
    ASSERT_TRUE(scripts) << scripts.error().message;
    const auto path = workspace / "postinst";
    auto code = std::error_code{};
    std::filesystem::create_directories(workspace, code);
    ASSERT_FALSE(code) << code.message();
    auto output = std::ofstream{path, std::ios::binary | std::ios::trunc};
    output << scripts->post_install;
    output.close();
    ASSERT_TRUE(output);
    const auto command = "/bin/sh '" + path.string() + "' configure";
    EXPECT_EQ(std::system(command.c_str()), 0);
    EXPECT_EQ(std::system(command.c_str()), 0);
    EXPECT_EQ(std::filesystem::read_symlink(workspace / "current"), workspace / "target");
}

TEST(Projection, generated_scripts_are_packaged_and_bound_to_attestations) {
    auto fixture = projection_fixture{};
    const auto intent = install_intent_fixture();
    for (const auto target : {liquid::projection_target::deb,
                              liquid::projection_target::rpm,
                              liquid::projection_target::apk}) {
        auto request = fixture.request(target, "scripts-" + std::string{liquid::to_string(target)});
        request.metadata.declared_dependencies.clear();
        request.install = intent;
        auto report = fixture.project(std::move(request));
        ASSERT_EQ(report.attestations.size(), 2U);
        auto expected = liquid::generate_maintainer_scripts(target, intent);
        ASSERT_TRUE(expected) << expected.error().message;
        const auto bytes = read_bytes(report.artifacts.front().path);
        auto archive = liquid::format_registry::global().unpack(
            bytes, report.artifacts.front().path.filename().string());
        ASSERT_TRUE(archive) << archive.error().message;
        expect_packaged_scripts(*archive, target, *expected);
        auto projection_statement = liquid::runtime::block_on(
            fixture.repository().attestations({.id = report.attestations.front()}));
        ASSERT_TRUE(projection_statement) << projection_statement.error().message;
        ASSERT_EQ(projection_statement->records.size(), 1U);
        EXPECT_EQ(projection_statement->records.front().value.profile, "projection/1");
        EXPECT_TRUE(std::ranges::contains(projection_statement->records.front().value.attributes,
                                          "scripts",
                                          &liquid::attestation_attribute::name));
        auto sbom_statement = liquid::runtime::block_on(
            fixture.repository().attestations({.id = report.attestations.back()}));
        ASSERT_TRUE(sbom_statement) << sbom_statement.error().message;
        ASSERT_EQ(sbom_statement->records.size(), 1U);
        EXPECT_EQ(sbom_statement->records.front().value.profile, "sbom/1");
    }
}

TEST(Projection, batch_conflicts_name_both_packages_and_every_path) {
    auto fixture = projection_fixture{};
    auto first = fixture.request(liquid::projection_target::deb, "conflict-first");
    auto second = fixture.request(liquid::projection_target::deb, "conflict-second");
    first.metadata.name = "package-a";
    second.metadata.name = "package-b";
    auto refused = liquid::runtime::block_on(
        liquid::project(fixture.repository(), std::vector{first, second}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::conflict);
    EXPECT_NE(refused.error().message.find("package-a"), std::string::npos);
    EXPECT_NE(refused.error().message.find("package-b"), std::string::npos);
    EXPECT_NE(refused.error().message.find("/usr/bin/liquid-fixture"), std::string::npos);
    EXPECT_NE(refused.error().message.find("/usr/etc/liquid-projection.conf"), std::string::npos);

    second.metadata.replaces.push_back("package-a");
    auto replaced = liquid::runtime::block_on(
        liquid::project(fixture.repository(), std::vector{first, second}));
    ASSERT_TRUE(replaced) << replaced.error().message;
    ASSERT_EQ(replaced->size(), 2U);
    ASSERT_EQ(replaced->back().replacements.size(), 1U);
    EXPECT_EQ(replaced->back().replacements.front().package, "package-a");
    EXPECT_TRUE(std::ranges::contains(replaced->back().replacements.front().paths,
                                      "/usr/bin/liquid-fixture"));
}

TEST(Projection, against_package_directory_detects_existing_ownership) {
    auto fixture = projection_fixture{};
    auto first = fixture.request(liquid::projection_target::deb, "against");
    first.metadata.name = "installed-package";
    auto installed = fixture.project(std::move(first));
    auto second = fixture.request(liquid::projection_target::deb, "against-new");
    second.metadata.name = "new-package";
    second.options.against = installed.artifacts.front().path.parent_path();
    auto refused =
        liquid::runtime::block_on(liquid::project(fixture.repository(), std::move(second)));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::conflict);
    EXPECT_NE(refused.error().message.find("installed-package"), std::string::npos);
    EXPECT_NE(refused.error().message.find("new-package"), std::string::npos);
}

TEST(Projection, against_dpkg_status_and_rpm_dump_detects_owned_paths) {
    auto fixture = projection_fixture{};
    const auto workspace = liquid::test::fixture_path("liquid-projection-inventory");
    auto code = std::error_code{};
    std::filesystem::create_directories(workspace / "dpkg/info", code);
    ASSERT_FALSE(code) << code.message();
    {
        auto status = std::ofstream{workspace / "dpkg/status"};
        status << "Package: installed-deb\nStatus: install ok installed\n";
        auto paths = std::ofstream{workspace / "dpkg/info/installed-deb.list"};
        paths << "/usr/bin/liquid-fixture\n";
    }
    {
        auto request = fixture.request(liquid::projection_target::deb, "against-dpkg");
        request.metadata.name = "incoming-deb";
        request.options.against = workspace / "dpkg/status";
        auto refused =
            liquid::runtime::block_on(liquid::project(fixture.repository(), std::move(request)));
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, liquid::errc::conflict);
        EXPECT_NE(refused.error().message.find("installed-deb"), std::string::npos);
        EXPECT_NE(refused.error().message.find("incoming-deb"), std::string::npos);
    }
    {
        auto dump = std::ofstream{workspace / "rpm.dump"};
        dump << "installed-rpm /usr/bin/liquid-fixture\n";
    }
    {
        auto request = fixture.request(liquid::projection_target::rpm, "against-rpm");
        request.metadata.name = "incoming-rpm";
        request.options.against = workspace / "rpm.dump";
        auto refused =
            liquid::runtime::block_on(liquid::project(fixture.repository(), std::move(request)));
        ASSERT_FALSE(refused);
        EXPECT_EQ(refused.error().code, liquid::errc::conflict);
        EXPECT_NE(refused.error().message.find("installed-rpm"), std::string::npos);
        EXPECT_NE(refused.error().message.find("incoming-rpm"), std::string::npos);
    }
}

TEST(Projection, replacement_converts_against_conflict_to_package_relationships) {
    auto fixture = projection_fixture{};
    const auto workspace = liquid::test::fixture_path("liquid-projection-replaces");
    auto code = std::error_code{};
    std::filesystem::create_directories(workspace / "info", code);
    ASSERT_FALSE(code) << code.message();
    {
        auto status = std::ofstream{workspace / "status"};
        status << "Package: old-package\nStatus: install ok installed\n";
        auto paths = std::ofstream{workspace / "info/old-package.list"};
        paths << "/usr/bin/liquid-fixture\n";
    }
    auto request = fixture.request(liquid::projection_target::deb, "replaces-dpkg");
    request.metadata.name = "new-package";
    request.metadata.replaces.push_back("old-package");
    request.options.against = workspace / "status";
    auto report = fixture.project(std::move(request));
    ASSERT_EQ(report.replacements.size(), 1U);
    EXPECT_EQ(report.replacements.front().package, "old-package");
    EXPECT_TRUE(
        std::ranges::contains(report.replacements.front().paths, "/usr/bin/liquid-fixture"));
    auto archive =
        liquid::format_registry::global().unpack(read_bytes(report.artifacts.front().path),
                                                 report.artifacts.front().path.filename().string());
    ASSERT_TRUE(archive) << archive.error().message;
    const auto metadata = std::ranges::find(
        archive->entries, std::string_view{".metadata"}, &liquid::format_entry::path);
    ASSERT_NE(metadata, archive->entries.end());
    const auto control =
        std::string{reinterpret_cast<const char*>(metadata->bytes.data()), metadata->bytes.size()};
    EXPECT_NE(control.find("Conflicts: liquid-fixture-old, old-package"), std::string::npos)
        << control;
    EXPECT_NE(control.find("Replaces: liquid-fixture-preview, old-package"), std::string::npos)
        << control;
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
        request.install = install_intent_fixture();
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
