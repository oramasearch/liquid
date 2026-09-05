#include <gtest/gtest.h>

#include "liquid/resolve.hpp"

namespace {

auto sample_digest(unsigned char first) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{first};
    return output;
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

} // namespace
