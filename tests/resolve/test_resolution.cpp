#include <gtest/gtest.h>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <print>
#include <stop_token>

#include "liquid/repository.hpp"
#include "liquid/resolve.hpp"

#include "../temporary_directory.hpp"

namespace {

auto sample_digest(unsigned char first) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{first};
    return output;
}

auto run_resolution(liquid::package_universe universe,
                    std::vector<liquid::spec> roots,
                    liquid::resolve_options options = {}) -> liquid::result<liquid::resolution> {
    return liquid::runtime::block_on(
        liquid::resolve(std::move(universe), std::move(roots), std::move(options)));
}

auto parse(std::string_view text) -> liquid::spec {
    auto parsed = liquid::parse_spec(text);
    EXPECT_TRUE(parsed) << parsed.error().message;
    return parsed ? std::move(*parsed) : liquid::spec{};
}

auto node(const liquid::resolution_graph& graph, std::string_view name)
    -> const liquid::concretised_spec* {
    const auto found = std::ranges::find(graph.nodes, name, &liquid::concretised_spec::name);
    return found == graph.nodes.end() ? nullptr : &*found;
}

auto write_package(const std::filesystem::path& path, const liquid::package_record& package)
    -> void {
    auto encoded = liquid::encode_package(package);
    ASSERT_TRUE(encoded) << encoded.error().message;
    std::filesystem::create_directories(path.parent_path());
    auto output = std::ofstream{path, std::ios::binary};
    ASSERT_TRUE(output);
    output.write(reinterpret_cast<const char*>(encoded->data()),
                 static_cast<std::streamsize>(encoded->size()));
}

TEST(ResolutionSpec, round_trips_every_modifier) {
    const auto input = "hdf5@1.14.3+mpi~fortran%gcc@13.2 target=zen4 mode=threaded ^openmpi@5+cuda";
    auto parsed = liquid::parse_spec(input);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_EQ(parsed->name, "hdf5");
    EXPECT_EQ(parsed->version, "1.14.3");
    EXPECT_EQ(parsed->variants.at("mpi"), "true");
    EXPECT_EQ(parsed->variants.at("fortran"), "false");
    EXPECT_EQ(parsed->variants.at("mode"), "threaded");
    ASSERT_TRUE(parsed->compiler);
    EXPECT_EQ(parsed->compiler->name, "gcc");
    EXPECT_EQ(parsed->compiler->version, "13.2");
    EXPECT_EQ(parsed->target, "zen4");
    ASSERT_EQ(parsed->dependencies.size(), 1U);
    EXPECT_EQ(parsed->dependencies.front().name, "openmpi");
    EXPECT_EQ(parsed->dependencies.front().version, "5");

    auto reparsed = liquid::parse_spec(liquid::to_string(*parsed));
    ASSERT_TRUE(reparsed) << reparsed.error().message;
    EXPECT_EQ(*reparsed, *parsed);
}

TEST(ResolutionSpec, errors_name_the_one_based_column_and_expectation) {
    auto missing_version = liquid::parse_spec("hdf5@");
    ASSERT_FALSE(missing_version);
    EXPECT_EQ(missing_version.error().message, "column 6: expected version constraint after '@'");

    auto missing_value = liquid::parse_spec("hdf5 mode=");
    ASSERT_FALSE(missing_value);
    EXPECT_EQ(missing_value.error().message, "column 11: expected assignment value");
}

TEST(ResolutionRecords, package_record_is_canonical_and_round_trips) {
    const auto package = liquid::package_record{
        .name = "hdf5",
        .versions = {{"1.14.3", sample_digest(1)}, {"1.12.2", std::nullopt}},
        .variants = {{"mpi", {"false", "true"}, "false", std::nullopt},
                     {"api", {"v18", "v110"}, "v110", "+mpi"}},
        .dependencies = {{"mpi", true, "3:", "+mpi", liquid::dependency_type::link}},
        .provides = {{"data-format", "1"}},
        .conflicts = {{liquid::spec{
                           .name = "hdf5", .version = ":1.12", .variants = {{"mpi", "true"}}},
                       "+mpi",
                       "MPI requires HDF5 1.14 or newer"}},
        .compiler_constraints = {"gcc@12:", "clang@17:"},
        .target_constraints = {"x86_64", "zen4"},
        .patches = {{sample_digest(2), "@1.12.2"}}};
    auto encoded = liquid::encode_package(package);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::decode_package(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    auto reencoded = liquid::encode_package(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
    EXPECT_EQ(decoded->name, package.name);
    EXPECT_EQ(decoded->versions.size(), package.versions.size());
    EXPECT_EQ(decoded->dependencies.front().type, liquid::dependency_type::link);
    EXPECT_EQ(decoded->conflicts.front().constraint.name, "hdf5");
}

TEST(ResolutionRecords, concretised_identity_covers_bound_edges) {
    const auto value = liquid::concretised_spec{
        .name = "hdf5",
        .version = "1.14.3",
        .variants = {{"mpi", "true"}},
        .compiler = sample_digest(3),
        .target = "zen4",
        .dependencies = {{"mpi", liquid::dependency_type::link, sample_digest(4), true}},
        .patches = {sample_digest(5)},
        .reasons = {"selected by root hdf5@1.14.3+mpi"}};
    auto encoded = liquid::encode_concretised_spec(value);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto decoded = liquid::decode_concretised_spec(*encoded);
    ASSERT_TRUE(decoded) << decoded.error().message;
    EXPECT_EQ(decoded->name, value.name);
    EXPECT_EQ(decoded->dependencies.front().name, "mpi");
    EXPECT_NE(liquid::to_string(decoded->id), liquid::to_string(sample_digest(0)));
    auto reencoded = liquid::encode_concretised_spec(*decoded);
    ASSERT_TRUE(reencoded) << reencoded.error().message;
    EXPECT_EQ(*reencoded, *encoded);
}

TEST(ResolutionRecords, directory_universe_loads_the_documented_packages_layout) {
    const auto location = liquid::test::fixture_path("resolution-directory-universe");
    std::filesystem::remove_all(location);
    write_package(location / "packages" / "h" / "hdf5.cbor",
                  liquid::package_record{.name = "hdf5", .versions = {{"1.14.3", std::nullopt}}});
    write_package(location / "packages" / "m" / "mpich.cbor",
                  liquid::package_record{.name = "mpich",
                                         .versions = {{"4.2", std::nullopt}},
                                         .provides = {{"mpi", "4"}}});
    auto loaded = liquid::runtime::block_on(liquid::load_package_universe(location));
    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded->packages.size(), 2U);
    EXPECT_EQ(loaded->packages[0].name, "hdf5");
    EXPECT_EQ(loaded->packages[1].name, "mpich");
    std::filesystem::remove_all(location);
}

TEST(ResolutionRecords, repository_universe_loads_package_blobs) {
    const auto location = liquid::test::fixture_path("resolution-repository-universe");
    std::filesystem::remove_all(location);
    {
        auto repository = liquid::runtime::block_on(liquid::repository::create(location));
        ASSERT_TRUE(repository) << repository.error().message;
        auto encoded = liquid::encode_package(liquid::package_record{
            .name = "app",
            .versions = {{"1", std::nullopt}},
        });
        ASSERT_TRUE(encoded) << encoded.error().message;
        auto written = liquid::runtime::block_on(repository->write("packages/app.cbor", *encoded));
        ASSERT_TRUE(written) << written.error().message;
        auto loaded = liquid::runtime::block_on(liquid::load_package_universe(*repository));
        ASSERT_TRUE(loaded) << loaded.error().message;
        ASSERT_EQ(loaded->packages.size(), 1U);
        EXPECT_EQ(loaded->packages.front().name, "app");
    }
    std::filesystem::remove_all(location);
}

TEST(ResolutionRecords, declarations_mix_with_records_and_includes_load_once) {
    const auto location = liquid::test::fixture_path("resolution-declarations");
    std::filesystem::create_directories(location / "packages" / "nested");
    std::ofstream{location / "packages" / "app.liquid"}
        << "include \"nested/library.liquid\"\n"
           "package app { versions: [1.0] depends_on library { version: 2.0 } }\n";
    std::ofstream{location / "packages" / "nested" / "library.liquid"}
        << "package library { versions: [1.0, 2.0] }\n";
    write_package(location / "packages" / "other.cbor",
                  {.name = "other", .versions = {{"1.0", std::nullopt}}});
    auto loaded = liquid::runtime::block_on(liquid::load_package_universe(location));
    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded->packages.size(), 3U);
    auto solved = run_resolution(*loaded, {parse("app")});
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    ASSERT_NE(graph, nullptr);
    const auto* library = node(*graph, "library");
    ASSERT_NE(library, nullptr);
    EXPECT_EQ(library->version, "2.0");
    std::filesystem::remove_all(location);
}

TEST(ResolutionRecords, declaration_errors_and_duplicates_are_not_ignored) {
    const auto location = liquid::test::fixture_path("resolution-declaration-errors");
    std::filesystem::create_directories(location);
    const auto path = location / "bad.liquid";
    std::ofstream{path} << "package bad { versions: [\"1.0\"] }\n";
    auto loaded = liquid::runtime::block_on(liquid::load_package_universe(location));
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().message.find("bad.liquid:1:26: expected version"), std::string::npos);
    std::ofstream{path} << "package bad { versions: [1.0] }\n";
    write_package(location / "duplicate.cbor",
                  {.name = "bad", .versions = {{"1.0", std::nullopt}}});
    loaded = liquid::runtime::block_on(liquid::load_package_universe(location));
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().message, "duplicate package 'bad'");
    std::filesystem::remove(location / "duplicate.cbor");
    std::ofstream{location / "duplicate.liquid"} << "package bad { versions: [2.0] }\n";
    loaded = liquid::runtime::block_on(liquid::load_package_universe(location));
    ASSERT_FALSE(loaded);
    EXPECT_EQ(loaded.error().message, "duplicate package 'bad'");
    std::filesystem::remove_all(location);
}

TEST(ResolutionRecords, canonical_companions_preserve_existing_universe_loading) {
    const auto location = liquid::test::fixture_path("resolution-canonical-companions");
    std::filesystem::create_directories(location);
    std::ofstream{location / "app.liquid"}
        << "include \"library.liquid\"\npackage app { versions: [1.0] }\n";
    std::ofstream{location / "library.liquid"} << "package library { versions: [2.0] }\n";
    write_package(location / "library.cbor",
                  {.name = "library", .versions = {{"1.0", std::nullopt}}});
    auto loaded = liquid::runtime::block_on(liquid::load_package_universe(location));
    ASSERT_TRUE(loaded) << loaded.error().message;
    ASSERT_EQ(loaded->packages.size(), 2U);
    EXPECT_EQ(loaded->packages[1].name, "library");
    EXPECT_EQ(loaded->packages[1].versions.front().version, "1.0");
    std::filesystem::remove_all(location);
}

TEST(ResolutionRecords, repository_declarations_resolve_with_canonical_records) {
    const auto location = liquid::test::fixture_path("resolution-repository-declarations");
    {
        auto repository = liquid::runtime::block_on(liquid::repository::create(location));
        ASSERT_TRUE(repository) << repository.error().message;
        const auto declaration =
            std::string{"package app { versions: [1.0] depends_on library { version: 2.0 } }\n"};
        const auto bytes = std::as_bytes(std::span{declaration});
        auto written = liquid::runtime::block_on(repository->write(
            "packages/app.liquid", std::vector<std::byte>{bytes.begin(), bytes.end()}));
        ASSERT_TRUE(written) << written.error().message;
        auto encoded =
            liquid::encode_package({.name = "library", .versions = {{"2.0", std::nullopt}}});
        ASSERT_TRUE(encoded);
        written = liquid::runtime::block_on(repository->write("packages/library.cbor", *encoded));
        ASSERT_TRUE(written) << written.error().message;
        auto loaded = liquid::runtime::block_on(liquid::load_package_universe(*repository));
        ASSERT_TRUE(loaded) << loaded.error().message;
        auto solved = run_resolution(*loaded, {parse("app")});
        ASSERT_TRUE(solved) << solved.error().message;
        const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
        ASSERT_NE(graph, nullptr);
        EXPECT_EQ(graph->nodes.size(), 2U);
    }
    std::filesystem::remove_all(location);
}

TEST(ResolutionSolver, selects_a_versioned_virtual_provider_and_bound_variant) {
    const auto universe = liquid::package_universe{
        {{.name = "app",
          .versions = {{"1", std::nullopt}},
          .variants = {{"mpi", {"false", "true"}, "false", std::nullopt}},
          .dependencies = {{"mpi", true, "3:", "+mpi", liquid::dependency_type::link}}},
         {.name = "mpich", .versions = {{"4.2", std::nullopt}}, .provides = {{"mpi", "4"}}},
         {.name = "openmpi", .versions = {{"5.0", std::nullopt}}, .provides = {{"mpi", "3:5"}}}}};
    auto solved = run_resolution(universe, {parse("app+mpi ^openmpi@5")});
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    ASSERT_NE(graph, nullptr);
    const auto* app = node(*graph, "app");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->variants.at("mpi"), "true");
    ASSERT_EQ(app->dependencies.size(), 1U);
    EXPECT_EQ(app->dependencies.front().name, "mpi");
    EXPECT_TRUE(app->dependencies.front().virtual_dependency);
    EXPECT_NE(node(*graph, "openmpi"), nullptr);
}

TEST(ResolutionSolver, binds_the_compiler_as_a_build_dependency) {
    const auto universe = liquid::package_universe{
        {{.name = "app", .versions = {{"1", std::nullopt}}, .compiler_constraints = {"gcc@13:"}},
         {.name = "gcc", .versions = {{"13.2", std::nullopt}, {"12.3", std::nullopt}}}}};
    auto solved = run_resolution(universe, {parse("app%gcc@13.2")});
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    ASSERT_NE(graph, nullptr);
    const auto* app = node(*graph, "app");
    const auto* gcc = node(*graph, "gcc");
    ASSERT_NE(app, nullptr);
    ASSERT_NE(gcc, nullptr);
    ASSERT_TRUE(app->compiler);
    EXPECT_EQ(liquid::to_string(*app->compiler), liquid::to_string(gcc->id));
    ASSERT_EQ(app->dependencies.size(), 1U);
    EXPECT_EQ(app->dependencies.front().type, liquid::dependency_type::build);
}

TEST(ResolutionSolver, conflict_core_names_both_constraint_origins) {
    const auto universe = liquid::package_universe{
        {{.name = "app",
          .versions = {{"1", std::nullopt}},
          .dependencies = {{"library", false, "2:", std::nullopt, liquid::dependency_type::link}}},
         {.name = "library", .versions = {{"1", std::nullopt}}}}};
    auto solved = run_resolution(universe, {parse("app@1")});
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* unsat = std::get_if<liquid::unsatisfiable>(&*solved);
    ASSERT_NE(unsat, nullptr);
    ASSERT_EQ(unsat->core.size(), 2U);
    EXPECT_EQ(unsat->core[0].origin.kind, liquid::constraint_origin_kind::root);
    EXPECT_EQ(unsat->core[1].origin.kind, liquid::constraint_origin_kind::dependency);
    EXPECT_NE(unsat->narrative.find("root app"), std::string::npos);
    EXPECT_NE(unsat->narrative.find("dependency app"), std::string::npos);
    auto json = liquid::resolution_json(*solved);
    ASSERT_TRUE(json);
    EXPECT_EQ(*json, unsat->json);
    EXPECT_EQ(
        *json,
        "{\"version\":1,\"unsatisfiable\":true,\"core\":[{\"kind\":\"root\",\"package\":\"app\""
        ",\"constraint\":\"root requires app@1\",\"detail\":\"root requires app@1\"},{\"kind"
        "\":\"dependency\",\"package\":\"app\",\"constraint\":\"app requires library@2:\","
        "\"detail\":\"app requires library@2:\"}],\"narrative\":\"Resolution is unsatisfiable."
        "\\n- root app: root requires app@1\\n- dependency app: app requires library@2:\\nNo "
        "configuration satisfies all 2 constraints.\"}");
}

TEST(ResolutionSolver, reuse_wins_over_a_newer_unbuilt_version) {
    const auto universe = liquid::package_universe{
        {{.name = "app",
          .versions = {{"2.0", std::nullopt}, {"1.0", std::nullopt}},
          .variants = {{"shared", {"false", "true"}, "true", std::nullopt}}}}};
    auto old_solve = run_resolution(universe, {parse("app@1.0")});
    ASSERT_TRUE(old_solve) << old_solve.error().message;
    const auto* old_graph = std::get_if<liquid::resolution_graph>(&*old_solve);
    ASSERT_NE(old_graph, nullptr);
    const auto* old_app = node(*old_graph, "app");
    ASSERT_NE(old_app, nullptr);

    auto options = liquid::resolve_options{.reuse = {old_app->id}};
    auto reused = run_resolution(universe, {parse("app")}, std::move(options));
    ASSERT_TRUE(reused) << reused.error().message;
    const auto* reused_graph = std::get_if<liquid::resolution_graph>(&*reused);
    ASSERT_NE(reused_graph, nullptr);
    const auto* reused_app = node(*reused_graph, "app");
    ASSERT_NE(reused_app, nullptr);
    EXPECT_EQ(reused_app->version, "1.0");
    ASSERT_FALSE(reused_app->reasons.empty());
    EXPECT_EQ(reused_app->reasons.front(), "reused already-built concretisation");

    auto newest = run_resolution(universe, {parse("app")});
    ASSERT_TRUE(newest) << newest.error().message;
    const auto* newest_graph = std::get_if<liquid::resolution_graph>(&*newest);
    ASSERT_NE(newest_graph, nullptr);
    const auto* newest_app = node(*newest_graph, "app");
    ASSERT_NE(newest_app, nullptr);
    EXPECT_EQ(newest_app->version, "2.0");

    auto ordered = run_resolution(
        universe,
        {parse("app")},
        liquid::resolve_options{.reuse = {old_app->id}, .known_outputs = {newest_app->id}});
    ASSERT_TRUE(ordered) << ordered.error().message;
    const auto* ordered_graph = std::get_if<liquid::resolution_graph>(&*ordered);
    ASSERT_NE(ordered_graph, nullptr);
    ASSERT_NE(node(*ordered_graph, "app"), nullptr);
    EXPECT_EQ(node(*ordered_graph, "app")->version, "1.0");
}

TEST(ResolutionSolver, known_identical_output_wins_after_reuse) {
    const auto universe = liquid::package_universe{
        {{.name = "app", .versions = {{"2.0", std::nullopt}, {"1.0", std::nullopt}}}}};
    auto old_solve = run_resolution(universe, {parse("app@1.0")});
    ASSERT_TRUE(old_solve) << old_solve.error().message;
    const auto* old_graph = std::get_if<liquid::resolution_graph>(&*old_solve);
    ASSERT_NE(old_graph, nullptr);
    const auto* old_app = node(*old_graph, "app");
    ASSERT_NE(old_app, nullptr);

    auto options = liquid::resolve_options{.known_outputs = {old_app->id}};
    auto solved = run_resolution(universe, {parse("app")}, std::move(options));
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    ASSERT_NE(graph, nullptr);
    const auto* app = node(*graph, "app");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->version, "1.0");
    ASSERT_FALSE(app->reasons.empty());
    EXPECT_EQ(app->reasons.front(), "selected configuration with known identical output");
}

TEST(ResolutionSolver, abi_compatibility_fact_makes_a_newer_candidate_reusable) {
    const auto universe = liquid::package_universe{
        {{.name = "app", .versions = {{"2.0", std::nullopt}, {"1.0", std::nullopt}}}}};
    auto old_solve = run_resolution(universe, {parse("app@1.0")});
    auto new_solve = run_resolution(universe, {parse("app@2.0")});
    ASSERT_TRUE(old_solve);
    ASSERT_TRUE(new_solve);
    const auto* old_graph = std::get_if<liquid::resolution_graph>(&*old_solve);
    const auto* new_graph = std::get_if<liquid::resolution_graph>(&*new_solve);
    ASSERT_NE(old_graph, nullptr);
    ASSERT_NE(new_graph, nullptr);
    const auto* old_app = node(*old_graph, "app");
    const auto* new_app = node(*new_graph, "app");
    ASSERT_NE(old_app, nullptr);
    ASSERT_NE(new_app, nullptr);

    auto options = liquid::resolve_options{
        .reuse = {old_app->id},
        .compatibility = {{.built = old_app->id, .candidate = new_app->id, .compatible = true}},
    };
    auto solved = run_resolution(universe, {parse("app")}, std::move(options));
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    ASSERT_NE(graph, nullptr);
    const auto* app = node(*graph, "app");
    ASSERT_NE(app, nullptr);
    EXPECT_EQ(app->version, "2.0");
    ASSERT_FALSE(app->reasons.empty());
    EXPECT_EQ(app->reasons.front(), "reused ABI-compatible built concretisation");
}

TEST(ResolutionSolver, conflict_rule_origin_is_reported) {
    auto package = liquid::package_record{
        .name = "app",
        .versions = {{"1", std::nullopt}},
        .conflicts = {{parse("app@1"), std::nullopt, "app@1 is disabled"}},
    };
    const auto universe = liquid::package_universe{{std::move(package)}};
    auto solved = run_resolution(universe, {parse("app@1")});
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* unsat = std::get_if<liquid::unsatisfiable>(&*solved);
    ASSERT_NE(unsat, nullptr);
    ASSERT_EQ(unsat->core.size(), 2U);
    EXPECT_EQ(unsat->core[0].origin.kind, liquid::constraint_origin_kind::root);
    EXPECT_EQ(unsat->core[1].origin.kind, liquid::constraint_origin_kind::conflict);
    EXPECT_NE(unsat->narrative.find("app@1 is disabled"), std::string::npos);
}

TEST(ResolutionSolver, cancellation_is_observed_before_grounding) {
    auto cancellation = std::stop_source{};
    cancellation.request_stop();
    auto solved = run_resolution(liquid::package_universe{},
                                 {parse("app")},
                                 liquid::resolve_options{.stop = cancellation.get_token()});
    ASSERT_FALSE(solved);
    EXPECT_EQ(solved.error().code, liquid::errc::busy);
    EXPECT_EQ(solved.error().message, "resolution cancelled before grounding");
}

TEST(ResolutionSolver, variant_when_condition_disables_non_default_values) {
    const auto universe = liquid::package_universe{
        {{.name = "app",
          .versions = {{"1", std::nullopt}},
          .variants = {{"accelerated", {"false", "true"}, "false", "@2:"}}}}};
    auto selected = run_resolution(universe, {parse("app")});
    ASSERT_TRUE(selected) << selected.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*selected);
    ASSERT_NE(graph, nullptr);
    ASSERT_NE(node(*graph, "app"), nullptr);
    EXPECT_EQ(node(*graph, "app")->variants.at("accelerated"), "false");

    auto rejected = run_resolution(universe, {parse("app+accelerated")});
    ASSERT_TRUE(rejected) << rejected.error().message;
    EXPECT_NE(std::get_if<liquid::unsatisfiable>(&*rejected), nullptr);
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
TEST(ResolutionCli, prints_a_concretised_graph_and_stable_json) {
    const auto location = liquid::test::fixture_path("resolution-cli-universe");
    std::filesystem::remove_all(location);
    write_package(location / "packages" / "app.cbor",
                  liquid::package_record{.name = "app",
                                         .versions = {{"2", std::nullopt}, {"1", std::nullopt}}});
    const auto output = location / "resolution.json";
    const auto command = std::format("\"{}\" resolve app --universe \"{}\" --json >\"{}\"",
                                     LIQUID_CLI,
                                     location.string(),
                                     output.string());
    ASSERT_EQ(std::system(command.c_str()), 0);
    auto input = std::ifstream{output};
    const auto json = std::string{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_TRUE(json.starts_with("{\"version\":1,\"id\":\"blake3:")) << json;
    EXPECT_NE(json.find("\"name\":\"app\",\"package_version\":\"2\""), std::string::npos) << json;
    EXPECT_NE(json.find("\"reasons\":[\"selected by root app\""), std::string::npos) << json;
    std::filesystem::remove_all(location);
}

TEST(ResolutionCli, reads_a_liquid_repository_universe) {
    const auto location = liquid::test::fixture_path("resolution-cli-repository");
    std::filesystem::remove_all(location);
    {
        auto repository = liquid::runtime::block_on(liquid::repository::create(location));
        ASSERT_TRUE(repository) << repository.error().message;
        auto encoded = liquid::encode_package(liquid::package_record{
            .name = "app",
            .versions = {{"1", std::nullopt}},
        });
        ASSERT_TRUE(encoded) << encoded.error().message;
        auto written = liquid::runtime::block_on(repository->write("packages/app.cbor", *encoded));
        ASSERT_TRUE(written) << written.error().message;
    }
    const auto output = location / "resolution.txt";
    const auto command = std::format("\"{}\" resolve app --universe \"{}\" >\"{}\"",
                                     LIQUID_CLI,
                                     location.string(),
                                     output.string());
    ASSERT_EQ(std::system(command.c_str()), 0);
    auto input = std::ifstream{output};
    const auto text = std::string{std::istreambuf_iterator<char>{input}, {}};
    EXPECT_TRUE(text.starts_with("app@1 target=generic blake3:")) << text;
    std::filesystem::remove_all(location);
}
#endif

TEST(ResolutionSolver, target_falls_back_to_its_architecture_family) {
    const auto universe = liquid::package_universe{{{.name = "app",
                                                     .versions = {{"1", std::nullopt}},
                                                     .target_constraints = {"x86_64", "zen3"}}}};
    auto solved = run_resolution(universe, {parse("app target=zen4")});
    ASSERT_TRUE(solved) << solved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*solved);
    ASSERT_NE(graph, nullptr);
    ASSERT_NE(node(*graph, "app"), nullptr);
    EXPECT_EQ(node(*graph, "app")->target, "zen3");
}

TEST(ResolutionSolver, graph_and_json_are_deterministic) {
    const auto universe = liquid::package_universe{
        {{.name = "app",
          .versions = {{"2", std::nullopt}, {"1", std::nullopt}},
          .dependencies =
              {{"library", false, std::nullopt, std::nullopt, liquid::dependency_type::run}}},
         {.name = "library", .versions = {{"3", std::nullopt}, {"2", std::nullopt}}}}};
    auto first = run_resolution(universe, {parse("app")});
    auto second = run_resolution(universe, {parse("app")});
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    const auto* first_graph = std::get_if<liquid::resolution_graph>(&*first);
    const auto* second_graph = std::get_if<liquid::resolution_graph>(&*second);
    ASSERT_NE(first_graph, nullptr);
    ASSERT_NE(second_graph, nullptr);
    EXPECT_EQ(liquid::to_string(first_graph->id), liquid::to_string(second_graph->id));
    auto first_json = liquid::resolution_json(*first);
    auto second_json = liquid::resolution_json(*second);
    ASSERT_TRUE(first_json);
    ASSERT_TRUE(second_json);
    EXPECT_EQ(*first_json, *second_json);
    EXPECT_EQ(
        *first_json,
        "{\"version\":1,\"id\":\"blake3:262e822f35ffcea1d0777b8a59822d96469befb962591"
        "c78978bd574ec227757\",\"roots\":[\"blake3:aa39d424af8428ec8d55ac787b1dd60dd20c"
        "2883e72a5be5d9002519cbf3f2ff\"],\"nodes\":[{\"id\":\"blake3:aa39d424af8428ec8d55ac"
        "787b1dd60dd20c2883e72a5be5d9002519cbf3f2ff\",\"name\":\"app\",\"package_version\":"
        "\"2\",\"target\":\"generic\",\"variants\":{},\"compiler\":null,\"dependencies\":[{\"name"
        "\":\"library\",\"type\":\"run\",\"target\":\"blake3:7533a1b3b72f793bce8b30811fcf06927"
        "fba642ef5142c0143c352df6323ac07\",\"virtual\":false}],\"reasons\":[\"selected by root "
        "app\",\"selected newest satisfying version 2\"]},{\"id\":\"blake3:7533a1b3b72"
        "f793bce8b30811fcf06927fba642ef5142c0143c352df6323ac07\",\"name\":\"library\",\"pack"
        "age_version\":\"3\",\"target\":\"generic\",\"variants\":{},\"compiler\":null,\"dependenc"
        "ies\":[],\"reasons\":[\"selected as a dependency\",\"selected newest satisfying "
        "version 3\"]}]}");
    auto explanation = liquid::why(*first_graph, "library");
    ASSERT_TRUE(explanation);
    EXPECT_NE(explanation->narrative.find("selected newest satisfying version 3"),
              std::string::npos);
    EXPECT_EQ(explanation->json,
              "{\"version\":1,\"package\":\"library\",\"reasons\":[\"selected as a dependency\","
              "\"selected newest satisfying version 3\"],\"narrative\":\"library@3 was selected."
              "\\n- selected as a dependency\\n- selected newest satisfying version 3\"}");
}

TEST(ResolutionSolver, generated_five_hundred_package_universe_resolves_and_resolves_again) {
    auto universe = liquid::package_universe{};
    universe.packages.reserve(500);
    for (std::size_t index = 0; index != 500; ++index) {
        auto package = liquid::package_record{
            .name = std::format("package-{:03}", index),
            .versions = {{"2.0", std::nullopt}, {"1.0", std::nullopt}},
            .variants = {{"shared", {"false", "true"}, "true", std::nullopt}}};
        for (const auto offset : {std::size_t{1}, std::size_t{3}, std::size_t{11}}) {
            if (index + offset >= 500) {
                continue;
            }
            package.dependencies.push_back({std::format("package-{:03}", index + offset),
                                            false,
                                            "1:",
                                            std::nullopt,
                                            liquid::dependency_type::link});
        }
        universe.packages.push_back(std::move(package));
    }
    const auto cold_start = std::chrono::steady_clock::now();
    auto cold = run_resolution(universe, {parse("package-000+shared target=zen4")});
    const auto cold_elapsed = std::chrono::steady_clock::now() - cold_start;
    ASSERT_TRUE(cold) << cold.error().message;
    const auto* cold_graph = std::get_if<liquid::resolution_graph>(&*cold);
    ASSERT_NE(cold_graph, nullptr);
    ASSERT_EQ(cold_graph->nodes.size(), 500U);

    auto options = liquid::resolve_options{};
    for (const auto& selected : cold_graph->nodes) {
        options.reuse.push_back(selected.id);
    }
    const auto warm_start = std::chrono::steady_clock::now();
    auto warm =
        run_resolution(universe, {parse("package-000+shared target=zen4")}, std::move(options));
    const auto warm_elapsed = std::chrono::steady_clock::now() - warm_start;
    ASSERT_TRUE(warm) << warm.error().message;
    const auto* warm_graph = std::get_if<liquid::resolution_graph>(&*warm);
    ASSERT_NE(warm_graph, nullptr);
    EXPECT_EQ(liquid::to_string(warm_graph->id), liquid::to_string(cold_graph->id));
    std::println("resolution measurement: packages=500 cold_ms={} resolve_again_ms={}",
                 std::chrono::duration_cast<std::chrono::milliseconds>(cold_elapsed).count(),
                 std::chrono::duration_cast<std::chrono::milliseconds>(warm_elapsed).count());
}

TEST(ResolutionSolver, two_thousand_package_dependency_chain_builds_without_native_recursion) {
    // Every package depends on the next one, so the selected graph is one chain whose depth equals
    // the universe size. Graph construction runs on a runtime worker thread, which on macOS has a
    // 512 KiB stack; one native frame per link overflowed it long before this length.
    constexpr auto count = std::size_t{2000};
    auto universe = liquid::package_universe{};
    universe.packages.reserve(count);
    for (std::size_t index = 0; index != count; ++index) {
        auto package = liquid::package_record{.name = std::format("chain-{:04}", index),
                                              .versions = {{"1.0", std::nullopt}}};
        if (index + 1 != count) {
            package.dependencies.push_back({std::format("chain-{:04}", index + 1),
                                            false,
                                            std::nullopt,
                                            std::nullopt,
                                            liquid::dependency_type::link});
        }
        universe.packages.push_back(std::move(package));
    }
    auto resolved = run_resolution(std::move(universe), {parse("chain-0000")});
    ASSERT_TRUE(resolved) << resolved.error().message;
    const auto* graph = std::get_if<liquid::resolution_graph>(&*resolved);
    ASSERT_NE(graph, nullptr);
    ASSERT_EQ(graph->nodes.size(), count);
    const auto* head = node(*graph, "chain-0000");
    const auto* second = node(*graph, "chain-0001");
    const auto* tail = node(*graph, "chain-1999");
    ASSERT_NE(head, nullptr);
    ASSERT_NE(second, nullptr);
    ASSERT_NE(tail, nullptr);
    ASSERT_EQ(head->dependencies.size(), 1U);
    EXPECT_EQ(liquid::to_string(head->dependencies.front().target), liquid::to_string(second->id));
    EXPECT_TRUE(tail->dependencies.empty());
}

} // namespace
