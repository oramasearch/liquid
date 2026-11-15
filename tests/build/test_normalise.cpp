// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <string>
#include <string_view>
#include <vector>

#include "build/normalise.hpp"
#include "sandbox/environment.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    std::transform(value.begin(), value.end(), output.begin(), [](char character) {
        return static_cast<std::byte>(static_cast<unsigned char>(character));
    });
    return output;
}

void append_field(std::string& output, std::string value, std::size_t width) {
    output += value;
    output.append(width - value.size(), ' ');
}

auto archive(std::uint64_t time, std::uint64_t uid, std::uint64_t gid) -> std::vector<std::byte> {
    auto output = std::string{"!<arch>\n"};
    append_field(output, "member.o/", 16);
    append_field(output, std::to_string(time), 12);
    append_field(output, std::to_string(uid), 6);
    append_field(output, std::to_string(gid), 6);
    append_field(output, "100644", 8);
    append_field(output, "3", 10);
    output += "`\n";
    output += "obj";
    output += '\n';
    return bytes(output);
}

auto pyc(std::uint32_t timestamp, std::uint32_t source_size) -> std::vector<std::byte> {
    auto output = bytes("PYTH");
    for (auto shift = 0U; shift != 32U; shift += 8U) {
        output.push_back(std::byte{});
    }
    for (const auto value : {timestamp, source_size}) {
        for (auto shift = 0U; shift != 32U; shift += 8U) {
            output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
        }
    }
    const auto payload = bytes("same-code-object");
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

} // namespace

TEST(OutputNormaliser, ar_member_owner_group_time_and_mode_are_deterministic) {
    auto first = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::ar_archive, archive(1, 501, 20));
    auto second = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::ar_archive, archive(9'999, 1'000, 100));

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*first, *second);
}

TEST(OutputNormaliser, python_bytecode_uses_a_payload_hash_instead_of_source_time) {
    auto first = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::python_bytecode, pyc(1, 20));
    auto second = liquid::build_detail::normalise_output(
        liquid::build_detail::output_kind::python_bytecode, pyc(2, 40));

    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*first, *second);
    EXPECT_EQ(std::to_integer<unsigned int>((*first)[4]), 1U);
    EXPECT_EQ(std::to_integer<unsigned int>((*first)[5]), 0U);
}

TEST(OutputNormaliser, binary_and_text_kinds_need_no_byte_rewrite) {
    const auto elf = bytes("\x7f" "ELFpayload");
    const auto text = bytes("prefix=/liquid/work/fixed\n");

    EXPECT_EQ(liquid::build_detail::output_kind_for("unit.o", elf),
              liquid::build_detail::output_kind::elf_object);
    EXPECT_EQ(liquid::build_detail::output_kind_for("library.pc", text),
              liquid::build_detail::output_kind::text);
    EXPECT_EQ(*liquid::build_detail::normalise_output(
                  liquid::build_detail::output_kind::elf_object, elf),
              elf);
    EXPECT_EQ(*liquid::build_detail::normalise_output(
                  liquid::build_detail::output_kind::text, text),
              text);
}

TEST(OutputNormaliser, tree_modes_keep_only_portable_executability) {
    EXPECT_EQ(liquid::build_detail::normalised_output_mode(0100775), 0755U);
    EXPECT_EQ(liquid::build_detail::normalised_output_mode(0100660), 0644U);
}

TEST(OutputNormaliser, sandbox_rules_fix_text_and_archive_time_inputs) {
    const auto action = liquid::digest{liquid::algorithm::blake3, {}};
    const auto input = liquid::action_input{.platform = {"linux", "x86_64"}};
    auto environment = liquid::sandbox_detail::environment(
        input, action, std::chrono::sys_seconds{std::chrono::seconds{9'999}});
    ASSERT_TRUE(environment) << environment.error().message;
    EXPECT_EQ(environment->values.at("SOURCE_DATE_EPOCH"), "9999");
    EXPECT_EQ(environment->values.at("ZERO_AR_DATE"), "1");
    auto definition = liquid::sandbox_detail::definition(input.platform);
    ASSERT_TRUE(definition) << definition.error().message;
    EXPECT_EQ(definition->output_normalisers,
              liquid::build_detail::output_normaliser_rules());
    auto encoded = liquid::objects::encode(*definition);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto without_rules = *definition;
    without_rules.output_normalisers.clear();
    auto legacy = liquid::objects::encode(without_rules);
    ASSERT_TRUE(legacy) << legacy.error().message;
    EXPECT_NE(*encoded, *legacy);
}
