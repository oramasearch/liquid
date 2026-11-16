// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <fstream>
#include <ranges>
#include <string>
#include <vector>

#include "../temporary_directory.hpp"
#include "formats/binary/macho.hpp"
#include "liquid/abi.hpp"
#include "liquid/build.hpp"
#include "liquid/format.hpp"

namespace {

// Every nlist record points to one shared string, independently of width and byte order.
auto macho_symbols(std::size_t name_size,
                   std::uint32_t count,
                   bool is64 = true,
                   bool little = true,
                   std::uint8_t type = 0x0f) -> std::vector<std::byte> {
    const auto header_size = std::size_t{is64 ? 32U : 28U};
    const auto stride = std::size_t{is64 ? 16U : 12U};
    const auto symbols = header_size + 24;
    const auto strings = symbols + count * stride;
    auto bytes = std::vector<std::byte>(strings + name_size + 2);
    const auto put = [&](std::size_t offset, std::uint64_t value, std::size_t width = 4) {
        for (auto index = std::size_t{}; index < width; ++index) {
            const auto shift = (little ? index : width - index - 1) * 8U;
            bytes[offset + index] = static_cast<std::byte>((value >> shift) & 0xffU);
        }
    };
    put(0, is64 ? 0xfeedfacfU : 0xfeedfaceU);
    put(4, is64 ? 0x0100000cU : 12U);
    put(12, 1);
    put(16, 1);
    put(20, 24);
    put(header_size, 2); // LC_SYMTAB
    put(header_size + 4, 24);
    put(header_size + 8, symbols);
    put(header_size + 12, count);
    put(header_size + 16, strings);
    put(header_size + 20, name_size + 2);
    for (auto index = std::uint32_t{}; index < count; ++index) {
        const auto at = symbols + index * stride;
        put(at, 1);
        put(at + 4, type, 1);
        put(at + 5, 1, 1);
        put(at + 8, index, is64 ? 8 : 4);
    }
    std::fill_n(
        bytes.begin() + static_cast<std::ptrdiff_t>(strings + 1), name_size, std::byte{'x'});
    return bytes;
}

TEST(MachOSymbols, TruncatesLongNamesBeforeCopyingForEveryNlistEncoding) {
    for (const auto is64 : {false, true}) {
        for (const auto little : {false, true}) {
            // Exported, local, undefined external, and debugging symbols all populate the sample.
            for (const auto type : {0x0f, 0x0e, 0x01, 0xef}) {
                SCOPED_TRACE(::testing::Message() << is64 << "/" << little << "/" << type);
                auto parsed = liquid::formats::binary::inspect_macho(
                    macho_symbols(1U << 20U, 2, is64, little, type));
                ASSERT_TRUE(parsed) << parsed.error().message;
                EXPECT_TRUE(parsed->truncated);
                ASSERT_EQ(parsed->symbol_names.size(), 2U);
                EXPECT_EQ(parsed->symbol_names.front().size(), 64U << 10U);
                EXPECT_LE(parsed->symbol_bytes, 16U << 20U);
                ASSERT_EQ(parsed->exported_symbols.size(), type == 0x0f ? 2U : 0U);
                if (type == 0x0f) {
                    EXPECT_EQ(parsed->exported_symbols.front().name.size(), 64U << 10U);
                }
            }
        }
    }
}

TEST(MachOSymbols, ChargesEveryRepeatedOffsetAndBothRetainedCopies) {
    const auto source = macho_symbols(4096, 300);
    auto first = liquid::formats::binary::inspect_macho(macho_symbols(4096, 1));
    ASSERT_TRUE(first);
    ASSERT_FALSE(first->truncated);
    ASSERT_GE(first->symbol_bytes, 8192U);
    for (const auto budget : {std::uint64_t{0},
                              first->symbol_bytes - 1,
                              first->symbol_bytes,
                              first->symbol_bytes * 257}) {
        auto parsed = liquid::formats::binary::inspect_macho(source, budget);
        ASSERT_TRUE(parsed) << parsed.error().message;
        EXPECT_TRUE(parsed->truncated);
        EXPECT_LE(parsed->symbol_bytes, budget);
        // The 257th export is collected after the 256-name sample is full.
        const auto expected = budget < first->symbol_bytes    ? 0U
                              : budget == first->symbol_bytes ? 1U
                                                              : 257U;
        EXPECT_EQ(parsed->exported_symbols.size(), expected);
        EXPECT_EQ(parsed->symbol_names.size(), std::min(expected, 256U));
    }
}

TEST(MachOSymbols, CapsTheDefaultBudgetAndPreservesPartialAbiExports) {
    const auto source = macho_symbols(64U << 10U, 300);
    auto parsed = liquid::formats::binary::inspect_macho(source);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->truncated);
    EXPECT_LE(parsed->symbol_bytes, 16U << 20U);
    EXPECT_GT(parsed->exported_symbols.size(), 0U);
    EXPECT_LT(parsed->exported_symbols.size(), 300U);
    auto abi = liquid::extract_abi(source);
    ASSERT_TRUE(abi) << abi.error().message;
    EXPECT_EQ(abi->exported_symbols.size(), parsed->exported_symbols.size());
    EXPECT_TRUE(abi->truncated);
    auto json = liquid::abi_json(*abi);
    ASSERT_TRUE(json);
    EXPECT_NE(json->find("\"truncated\":true"), std::string::npos);
    const auto report = liquid::diff_abi(*abi, *abi);
    EXPECT_EQ(report.verdict, liquid::abi_verdict::incompatible);
    ASSERT_EQ(report.changes.size(), 1U);
    EXPECT_EQ(report.changes.front().kind, liquid::abi_change_kind::inspection_truncated);
    auto complete = liquid::extract_abi(macho_symbols(32, 1));
    ASSERT_TRUE(complete);
    EXPECT_FALSE(complete->truncated);
    EXPECT_EQ(liquid::diff_abi(*complete, *abi).verdict, liquid::abi_verdict::incompatible);
    EXPECT_EQ(liquid::diff_abi(*abi, *complete).verdict, liquid::abi_verdict::incompatible);
}

TEST(MachOSymbols, HonorsRegistryBudgetAndReportsTruncatedNames) {
    const auto codecs = liquid::format_registry::global().codecs();
    const auto found = std::ranges::find_if(
        codecs, [](const auto* codec) { return codec->descriptor().name == "macho"; });
    ASSERT_NE(found, codecs.end());
    const auto* codec = *found;
    auto bounded = codec->unpack(macho_symbols(4096, 300), {.byte_budget = 32U << 10U});
    ASSERT_TRUE(bounded) << bounded.error().message;
    EXPECT_TRUE(bounded->truncated);
    EXPECT_FALSE(bounded->limits.empty());
    EXPECT_GT(bounded->entries.size(), 0U);
    EXPECT_LT(bounded->entries.size(), 8U);

    auto long_name = codec->unpack(macho_symbols((64U << 10U) + 1, 1));
    ASSERT_TRUE(long_name) << long_name.error().message;
    EXPECT_TRUE(long_name->truncated);
    ASSERT_EQ(long_name->entries.size(), 1U);
    EXPECT_EQ(long_name->entries.front().path.size(), (64U << 10U) + 8);

    // A three-byte UTF-8 character straddles the 64 KiB cutoff; CBOR requires valid text.
    auto unicode = macho_symbols((64U << 10U) + 2, 1);
    unicode[unicode.size() - 4] = std::byte{0xe2};
    unicode[unicode.size() - 3] = std::byte{0x82};
    unicode[unicode.size() - 2] = std::byte{0xac};
    auto unicode_tree = codec->unpack(unicode);
    ASSERT_TRUE(unicode_tree) << unicode_tree.error().message;
    EXPECT_TRUE(unicode_tree->truncated);
    ASSERT_EQ(unicode_tree->entries.size(), 1U);
    EXPECT_EQ(unicode_tree->entries.front().path.size(), (64U << 10U) - 1 + 8);
}

TEST(MachOSymbols, PreservesExactLimitAndRejectsUnterminatedShortNames) {
    for (const auto length : {0U, 1U, 32U << 10U, 64U << 10U}) {
        auto parsed = liquid::formats::binary::inspect_macho(macho_symbols(length, 1));
        ASSERT_TRUE(parsed);
        EXPECT_FALSE(parsed->truncated);
        ASSERT_EQ(parsed->exported_symbols.size(), length == 0 ? 0U : 1U);
        if (length != 0) {
            EXPECT_EQ(parsed->exported_symbols.front().name, std::string(length, 'x'));
        }
    }
    auto source = macho_symbols(32, 1);
    source.back() = std::byte{'x'};
    auto parsed = liquid::formats::binary::inspect_macho(source);
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed->symbol_names.empty());
    EXPECT_TRUE(parsed->exported_symbols.empty());
}

TEST(MachOSymbols, ReportsTruncationInSbomAndRelocationConsumers) {
    const auto temporary = liquid::test::detail::process_fixture_directory{};
    const auto path = temporary.path() / "oversized.macho";
    const auto bytes = macho_symbols((64U << 10U) + 1, 1);
    {
        auto file = std::ofstream{path, std::ios::binary};
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
        ASSERT_TRUE(file);
    }
    for (const auto format : {liquid::sbom_format::spdx, liquid::sbom_format::cyclonedx}) {
        auto report = liquid::runtime::block_on(liquid::build::infer_sbom(path, format));
        ASSERT_TRUE(report) << report.error().message;
        EXPECT_NE(report->document.find("linked-binary-symbols:truncated"), std::string::npos);
    }
    auto relocation = liquid::runtime::block_on(liquid::build::relocation_check(temporary.path()));
    ASSERT_TRUE(relocation) << relocation.error().message;
    EXPECT_FALSE(relocation->relocatable);
    EXPECT_TRUE(std::ranges::any_of(relocation->findings, [](const auto& finding) {
        return finding.context == "Mach-O symbol inspection was truncated";
    }));
}

} // namespace
