#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <limits>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/format.hpp"

namespace {

auto fixture(std::string_view name) -> std::vector<std::byte> {
    const auto path = std::filesystem::path{LIQUID_FORMAT_FIXTURES} / name;
    auto input = std::ifstream{path, std::ios::binary};
    EXPECT_TRUE(input) << path;
    auto characters = std::vector<char>{std::istreambuf_iterator<char>{input}, {}};
    auto output = std::vector<std::byte>(characters.size());
    std::transform(characters.begin(), characters.end(), output.begin(), [](char value) {
        return static_cast<std::byte>(static_cast<unsigned char>(value));
    });
    return output;
}

auto codec(std::string_view name) -> const liquid::format_codec* {
    const auto codecs = liquid::format_registry::global().codecs();
    const auto found = std::ranges::find_if(
        codecs, [&](const auto* value) { return value->descriptor().name == name; });
    return found == codecs.end() ? nullptr : *found;
}

auto entry(const liquid::format_tree& tree, std::string_view path) -> const liquid::format_entry* {
    const auto found = std::ranges::find(tree.entries, path, &liquid::format_entry::path);
    return found == tree.entries.end() ? nullptr : &*found;
}

auto file_entry(std::string path, std::vector<std::byte> bytes) -> liquid::format_entry {
    auto content = liquid::hash(liquid::algorithm::blake3, bytes);
    EXPECT_TRUE(content) << content.error().message;
    return {.path = std::move(path),
            .kind = liquid::format_entry_kind::file,
            .size = bytes.size(),
            .content = content.value_or(liquid::digest{}),
            .bytes = std::move(bytes)};
}

// A one-row, uncompressed INT32 column with a dictionary and a caller-controlled data page.
auto parquet_page_fixture(std::span<const std::byte> payload,
                          std::uint8_t encoding,
                          std::uint8_t repetition = 0) -> std::vector<std::byte> {
    auto bytes = std::vector<std::byte>{};
    const auto append = [&](std::initializer_list<std::uint8_t> values) {
        for (const auto value : values) {
            bytes.push_back(static_cast<std::byte>(value));
        }
    };
    const auto varint = [&](std::uint64_t value) {
        while (value >= 128) {
            append({static_cast<std::uint8_t>((value & 0x7fU) | 0x80U)});
            value >>= 7U;
        }
        append({static_cast<std::uint8_t>(value)});
    };
    append({'P', 'A', 'R', '1'});
    // Dictionary page: one PLAIN integer, value zero.
    append({0x15, 4, 0x15, 8, 0x15, 8, 0x4c, 0x15, 2, 0x15, 0, 0, 0, 0, 0, 0, 0});
    const auto data_offset = bytes.size();
    append({0x15, 0, 0x15});
    varint(payload.size() * 2);
    append({0x15});
    varint(payload.size() * 2);
    append({0x2c, 0x15, 2, 0x15, static_cast<std::uint8_t>(encoding * 2), 0x15, 6, 0x15, 6, 0, 0});
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const auto footer_offset = bytes.size();
    // FileMetaData: version, schema (root + leaf), row count and one row group.
    append({0x15, 2,    0x19, 0x2c, 0x48, 1,    'r',
            0x15, 2,    0,    0x15, 2,    0x25, static_cast<std::uint8_t>(repetition * 2),
            0x18, 1,    'x',  0,    0x16, 2,    0x19,
            0x1c, 0x19, 0x1c, 0x3c, 0x15, 2,    0x29,
            0x18, 1,    'x',  0x15, 0,    0x26});
    varint((payload.size() + 4) * 2);
    append({0x36});
    varint(data_offset * 2);
    append({0x26, 8, 0, 0, 0x26, 2, 0, 0});
    const auto footer_size = bytes.size() - footer_offset;
    for (auto index = 0U; index < 4; ++index) {
        append({static_cast<std::uint8_t>((footer_size >> (index * 8U)) & 0xffU)});
    }
    append({'P', 'A', 'R', '1'});
    return bytes;
}

auto parquet_run(std::uint64_t header, std::size_t payload_bytes = 0) -> std::vector<std::byte> {
    auto bytes = std::vector<std::byte>{};
    while (header >= 128) {
        bytes.push_back(static_cast<std::byte>((header & 0x7fU) | 0x80U));
        header >>= 7U;
    }
    bytes.push_back(static_cast<std::byte>(header));
    bytes.resize(bytes.size() + payload_bytes);
    return bytes;
}

TEST(FormatRegistry, PublishesBuiltInCodecs) {
    const auto codecs = liquid::format_registry::global().codecs();
    auto names = std::vector<std::string>{};
    for (const auto* value : codecs) {
        names.push_back(value->descriptor().name);
    }
    std::ranges::sort(names);
    EXPECT_EQ(names,
              (std::vector<std::string>{"apk",
                                        "deb",
                                        "elf",
                                        "hdf5",
                                        "jar",
                                        "jupyter",
                                        "macho",
                                        "odf",
                                        "ooxml",
                                        "parquet",
                                        "pdf",
                                        "pe",
                                        "pickle",
                                        "rpm",
                                        "safetensors",
                                        "sqlite",
                                        "tar",
                                        "wheel",
                                        "zip"}));
    EXPECT_EQ(liquid::format_registry::global().find_by_extension("ARCHIVE.ZIP").size(), 1);
    EXPECT_EQ(liquid::format_registry::global().find_by_extension("data.parquet").size(), 1);
}

TEST(FormatRegistry, UnpacksNotebookTensorAndPickleContent) {
    const auto notebook_text =
        std::string{"{\"cells\":[{\"cell_type\":\"code\",\"execution_count\":7,\"outputs\":[{}],"
                    "\"source\":[\"print(1)\\n\"]}],\"nbformat\":4}"};
    const auto notebook = std::vector<std::byte>{
        reinterpret_cast<const std::byte*>(notebook_text.data()),
        reinterpret_cast<const std::byte*>(notebook_text.data() + notebook_text.size())};
    auto notebook_tree = liquid::format_registry::global().unpack(notebook, "sample.ipynb");
    ASSERT_TRUE(notebook_tree) << notebook_tree.error().message;
    EXPECT_EQ(notebook_tree->codec, "jupyter");
    ASSERT_NE(entry(*notebook_tree, "cells/0"), nullptr);

    const auto header =
        std::string{"{\"weight\":{\"data_offsets\":[0,2],\"dtype\":\"U8\",\"shape\":[2]}}"};
    auto tensors = std::vector<std::byte>(8 + header.size() + 2);
    for (auto index = std::size_t{}; index < 8; ++index) {
        tensors[index] = static_cast<std::byte>((header.size() >> (index * 8U)) & 0xffU);
    }
    std::copy(header.begin(), header.end(), reinterpret_cast<char*>(tensors.data() + 8));
    tensors[tensors.size() - 2] = std::byte{1};
    tensors[tensors.size() - 1] = std::byte{2};
    auto tensor_tree = liquid::format_registry::global().unpack(tensors, "sample.safetensors");
    ASSERT_TRUE(tensor_tree) << tensor_tree.error().message;
    const auto* tensor = entry(*tensor_tree, "tensors/weight");
    ASSERT_NE(tensor, nullptr);
    EXPECT_EQ(tensor->bytes.size(), 2U);

    const auto pickle_text = std::string{"cos\nsystem\n."};
    const auto pickle = std::vector<std::byte>{
        reinterpret_cast<const std::byte*>(pickle_text.data()),
        reinterpret_cast<const std::byte*>(pickle_text.data() + pickle_text.size())};
    auto pickle_tree = liquid::format_registry::global().unpack(pickle, "checkpoint.pkl");
    ASSERT_TRUE(pickle_tree) << pickle_tree.error().message;
    EXPECT_NE(entry(*pickle_tree, "globals/os.system"), nullptr);
}

TEST(FormatRegistry, UnpacksPdfObjectsAndExtractedText) {
    const auto source_text =
        std::string{"%PDF-1.4\n1 0 obj\n<< /Producer (builder) >>\nendobj\n2 0 obj\n"
                    "<< >>\nstream\nBT (hello) Tj ET\nendstream\nendobj\n%%EOF\n"};
    const auto source = std::vector<std::byte>{
        reinterpret_cast<const std::byte*>(source_text.data()),
        reinterpret_cast<const std::byte*>(source_text.data() + source_text.size())};
    auto tree = liquid::format_registry::global().unpack(source, "sample.pdf");
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_EQ(tree->codec, "pdf");
    EXPECT_NE(entry(*tree, "objects/1 0"), nullptr);
    const auto* extracted = entry(*tree, "text/2 0");
    ASSERT_NE(extracted, nullptr);
    const auto extracted_text = std::string_view{
        reinterpret_cast<const char*>(extracted->bytes.data()), extracted->bytes.size()};
    EXPECT_EQ(extracted_text, "hello\n");
}

TEST(FormatRegistry, LoadsSharedObjectsAcrossTheCAbi) {
    auto loaded = liquid::format_codec::load(LIQUID_FORMAT_TEST_MODULE);
    ASSERT_TRUE(loaded) << loaded.error().message << " " << loaded.error().context;
    EXPECT_EQ(loaded->descriptor().name, "test-module");
    EXPECT_EQ(loaded->descriptor().tier, liquid::format_tier::semantic_verbatim);
    EXPECT_EQ(loaded->descriptor().extensions, std::vector<std::string>{".fixture"});
    EXPECT_EQ(loaded->descriptor().limits.maximum_depth, 2U);
}

TEST(FormatRegistry, UnpacksZipAndRoundTripsStoredMembersExactly) {
    auto mixed = fixture("archive-a.zip");
    auto tree = liquid::format_registry::global().unpack(mixed, "archive-a.zip");
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_EQ(tree->codec, "zip");
    EXPECT_EQ(tree->tier, liquid::format_tier::semantic_verbatim);
    EXPECT_NE(entry(*tree, "stored.txt"), nullptr);
    EXPECT_NE(entry(*tree, "deflated.txt"), nullptr);
    EXPECT_NE(entry(*tree, "zip64.txt"), nullptr);

    auto stored = fixture("stored.zip");
    auto stored_tree = liquid::format_registry::global().unpack(stored, "stored.zip");
    ASSERT_TRUE(stored_tree) << stored_tree.error().message;
    ASSERT_EQ(stored_tree->tier, liquid::format_tier::canonical_round_trip);
    auto* zip = codec("zip");
    ASSERT_NE(zip, nullptr);
    auto repacked = zip->repack(*stored_tree);
    ASSERT_TRUE(repacked) << repacked.error().message;
    EXPECT_EQ(*repacked, stored);
}

TEST(FormatRegistry, UnpacksZipDataDescriptors) {
    auto source = fixture("descriptor.zip");
    auto tree = liquid::format_registry::global().unpack(source, "descriptor.zip");
    ASSERT_TRUE(tree) << tree.error().message;
    const auto* descriptor = entry(*tree, "descriptor.txt");
    ASSERT_NE(descriptor, nullptr);
    EXPECT_EQ(descriptor->bytes.size(), 19U);
}

TEST(FormatRegistry, UnpacksPaxTarAndWritesDeterministically) {
    auto source = fixture("pax.tar");
    auto tree = liquid::format_registry::global().unpack(source, "pax.tar");
    ASSERT_TRUE(tree) << tree.error().message;
    ASSERT_EQ(tree->entries.size(), 1);
    EXPECT_TRUE(tree->entries.front().path.starts_with("nested/long-directory-name/"));

    auto* tar = codec("tar");
    ASSERT_NE(tar, nullptr);
    auto original = tar->repack(*tree);
    ASSERT_TRUE(original) << original.error().message;
    EXPECT_EQ(*original, source);
    auto input =
        liquid::format_tree{.codec = "tar", .tier = liquid::format_tier::canonical_round_trip};
    input.entries.push_back(file_entry("z.txt", {std::byte{'z'}, std::byte{'\n'}}));
    input.entries.push_back(file_entry("a.txt", {std::byte{'a'}, std::byte{'\n'}}));
    auto first = tar->repack(input);
    auto second = tar->repack(input);
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_EQ(*first, *second);
    auto unpacked = liquid::format_registry::global().unpack(*first, "written.tar");
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    ASSERT_EQ(unpacked->entries.size(), 2);
    EXPECT_EQ(unpacked->entries[0].path, "a.txt");
    EXPECT_EQ(unpacked->entries[1].path, "z.txt");
}

TEST(FormatRegistry, UnpacksGzipAndZstdTarWrappersAsTierTwo) {
    for (const auto* name : {"pax.tar.gz", "pax.tar.zst"}) {
        auto tree = liquid::format_registry::global().unpack(fixture(name), name);
        ASSERT_TRUE(tree) << name << ": " << tree.error().message;
        EXPECT_EQ(tree->tier, liquid::format_tier::semantic_verbatim);
        ASSERT_EQ(tree->entries.size(), 1) << name;
        EXPECT_TRUE(tree->entries.front().path.starts_with("nested/"));
    }
}

TEST(FormatRegistry, UnpacksHdf5GroupsAndDatasets) {
    auto source = fixture("sample-a.h5");
    auto tree = liquid::format_registry::global().unpack(source, "sample-a.h5");
    ASSERT_TRUE(tree) << tree.error().message << " " << tree.error().context;
    EXPECT_EQ(tree->codec, "hdf5");
    EXPECT_NE(entry(*tree, "measurements"), nullptr);
    const auto* chunked = entry(*tree, "measurements/chunked");
    const auto* compact = entry(*tree, "measurements/compact");
    ASSERT_NE(chunked, nullptr);
    ASSERT_NE(compact, nullptr);
    EXPECT_EQ(chunked->kind, liquid::format_entry_kind::dataset);
    EXPECT_EQ(chunked->bytes.size(), 16U * sizeof(std::int32_t));
    EXPECT_EQ(compact->bytes.size(), 4U * sizeof(double));

    auto changed = fixture("sample-b.h5");
    auto changed_tree = liquid::format_registry::global().unpack(changed, "sample-b.h5");
    ASSERT_TRUE(changed_tree) << changed_tree.error().message;
    auto* hdf5 = codec("hdf5");
    ASSERT_NE(hdf5, nullptr);
    auto difference = hdf5->diff(*tree, *changed_tree);
    ASSERT_TRUE(difference) << difference.error().message;
    ASSERT_EQ(difference->entries.size(), 1);
    EXPECT_EQ(difference->entries.front().path, "measurements/chunked");
}

TEST(FormatRegistry, ComparesHdf5FloatingPointValuesWithTolerances) {
    auto original = liquid::format_registry::global().unpack(fixture("sample-a.h5"), "sample-a.h5");
    ASSERT_TRUE(original) << original.error().message;
    auto changed = *original;
    auto compact = std::ranges::find(
        changed.entries, std::string_view{"measurements/compact"}, &liquid::format_entry::path);
    ASSERT_NE(compact, changed.entries.end());
    auto value = double{};
    std::memcpy(&value, compact->bytes.data(), sizeof(value));
    value += 0.001;
    std::memcpy(compact->bytes.data(), &value, sizeof(value));
    auto content = liquid::hash(liquid::algorithm::blake3, compact->bytes);
    ASSERT_TRUE(content) << content.error().message;
    compact->content = *content;

    auto* hdf5 = codec("hdf5");
    ASSERT_NE(hdf5, nullptr);
    auto exact = hdf5->diff(*original, changed);
    ASSERT_TRUE(exact) << exact.error().message;
    ASSERT_EQ(exact->entries.size(), 1);
    auto tolerant =
        hdf5->diff(*original, changed, liquid::format_diff_options{.absolute_tolerance = 0.01});
    ASSERT_TRUE(tolerant) << tolerant.error().message;
    EXPECT_TRUE(tolerant->entries.empty());
}

TEST(FormatRegistry, UnpacksParquetRowGroupsAndDictionaryValues) {
    auto source = fixture("sample.parquet");
    auto tree = liquid::format_registry::global().unpack(source, "sample.parquet");
    ASSERT_TRUE(tree) << tree.error().message << " " << tree.error().context;
    EXPECT_EQ(tree->codec, "parquet");
    EXPECT_EQ(tree->entries.size(), 6);
    const auto* dictionary = entry(*tree, "row-group=0/category");
    ASSERT_NE(dictionary, nullptr);
    const auto text = std::string_view{reinterpret_cast<const char*>(dictionary->bytes.data()),
                                       dictionary->bytes.size()};
    EXPECT_NE(text.find("alpha"), std::string_view::npos);
    EXPECT_NE(text.find("beta"), std::string_view::npos);
}

TEST(FormatRegistry, RejectsParquetOversizedRunsOnEveryHybridPath) {
    for (const auto header : {(std::uint64_t{1} << 61U) + 1U,
                              (std::uint64_t{1} << 62U) + 3U,
                              std::numeric_limits<std::uint64_t>::max()}) {
        for (const auto encoding : {2U, 8U, 3U}) {
            for (const auto repetition : {0U, 1U, 2U}) {
                for (const auto trailing : {std::size_t{0}, std::size_t{2}}) {
                    SCOPED_TRACE(::testing::Message() << header << "/" << encoding << "/"
                                                      << repetition << "/" << trailing);
                    auto payload = parquet_run(header, trailing);
                    if (repetition != 0) {
                        // The malformed run is the first level stream (definition or repetition).
                        const auto size = payload.size();
                        payload.insert(payload.begin(), 4, std::byte{});
                        for (auto index = 0U; index < 4; ++index) {
                            payload[index] = static_cast<std::byte>((size >> (index * 8U)) & 0xffU);
                        }
                    } else if (encoding != 3) {
                        payload.insert(payload.begin(), std::byte{2});
                    }
                    auto tree = liquid::format_registry::global().unpack(
                        parquet_page_fixture(payload, encoding, repetition), "oversized.parquet");
                    ASSERT_FALSE(tree);
                    EXPECT_EQ(tree.error().code, liquid::errc::corrupt);
                }
            }
        }
    }
}

TEST(FormatRegistry, RejectsParquetInvalidWidthsAndVarints) {
    for (auto width = 65U; width <= 255U; ++width) {
        for (const auto header : {2U, 3U}) {
            auto payload = parquet_run(header, 256);
            payload.insert(payload.begin(), static_cast<std::byte>(width));
            auto tree = liquid::format_registry::global().unpack(parquet_page_fixture(payload, 8),
                                                                 "width.parquet");
            ASSERT_FALSE(tree) << width << "/" << header;
            EXPECT_EQ(tree.error().code, liquid::errc::corrupt);
        }
    }
    // The tenth varint byte has only one available bit; this used to alias header 3.
    auto payload = std::vector<std::byte>(10, std::byte{0x80});
    payload.front() = std::byte{0x83};
    payload.back() = std::byte{2};
    payload.insert(payload.begin(), std::byte{1});
    payload.push_back(std::byte{});
    auto tree = liquid::format_registry::global().unpack(parquet_page_fixture(payload, 8),
                                                         "varint.parquet");
    ASSERT_FALSE(tree);
    EXPECT_EQ(tree.error().code, liquid::errc::corrupt);
}

TEST(FormatRegistry, AcceptsParquetBoundedRunsAndRejectsTruncatedPayloads) {
    for (const auto width : {0U, 1U, 2U, 8U, 32U, 64U}) {
        for (const auto header : {2U, 3U}) {
            const auto size = header == 2 ? (width + 7U) / 8U : width;
            auto payload = parquet_run(header, size);
            payload.insert(payload.begin(), static_cast<std::byte>(width));
            auto tree = liquid::format_registry::global().unpack(parquet_page_fixture(payload, 8),
                                                                 "valid.parquet");
            ASSERT_TRUE(tree) << width << "/" << header << ": " << tree.error().message;
            ASSERT_NE(entry(*tree, "row-group=0/x"), nullptr);
            EXPECT_EQ(entry(*tree, "row-group=0/x")->bytes,
                      (std::vector<std::byte>{std::byte{'0'}, std::byte{'\n'}}));
            if (size != 0) {
                payload.pop_back();
                auto truncated = liquid::format_registry::global().unpack(
                    parquet_page_fixture(payload, 8), "truncated.parquet");
                ASSERT_FALSE(truncated) << width << "/" << header;
                EXPECT_EQ(truncated.error().code, liquid::errc::corrupt);
            }
        }
    }
}

TEST(FormatRegistry, UnpacksSqliteSchemaAndRows) {
    auto source = fixture("sample.sqlite");
    auto tree = liquid::format_registry::global().unpack(source, "sample.sqlite");
    ASSERT_TRUE(tree) << tree.error().message << " " << tree.error().context;
    EXPECT_EQ(tree->codec, "sqlite");
    EXPECT_NE(entry(*tree, "packages"), nullptr);
    EXPECT_NE(entry(*tree, "files"), nullptr);
    EXPECT_NE(entry(*tree, "packages/rowid=1"), nullptr);
    EXPECT_NE(entry(*tree, "files/rowid=2"), nullptr);
}

TEST(FormatRegistry, CanonicalisationStripsDeclaredVolatileFields) {
    auto first =
        liquid::format_registry::global().unpack(fixture("volatile-a.zip"), "volatile-a.zip");
    auto second =
        liquid::format_registry::global().unpack(fixture("volatile-b.zip"), "volatile-b.zip");
    ASSERT_TRUE(first) << first.error().message;
    ASSERT_TRUE(second) << second.error().message;
    auto* zip = codec("zip");
    ASSERT_NE(zip, nullptr);
    auto first_canonical = zip->canonicalise(*first);
    auto second_canonical = zip->canonicalise(*second);
    ASSERT_TRUE(first_canonical) << first_canonical.error().message;
    ASSERT_TRUE(second_canonical) << second_canonical.error().message;
    auto difference = zip->diff(*first_canonical, *second_canonical);
    ASSERT_TRUE(difference) << difference.error().message;
    EXPECT_TRUE(difference->entries.empty());
}

TEST(FormatRegistry, RejectsEveryRecognisedTruncationWithoutCrashing) {
    const auto cases = std::array{
        std::pair{"archive-a.zip", 4U},
        std::pair{"pax.tar", 512U},
        std::pair{"sample-a.h5", 8U},
        std::pair{"sample.parquet", 4U},
        std::pair{"sample.sqlite", 16U},
    };
    for (const auto& [name, signature_size] : cases) {
        const auto source = fixture(name);
        auto accepted_prefix = source.size();
        for (auto size = signature_size; size < source.size(); ++size) {
            auto truncated = liquid::format_registry::global().unpack(
                std::span<const std::byte>{source}.first(size), name);
            if (truncated) {
                accepted_prefix = size;
                break;
            }
        }
        EXPECT_TRUE(liquid::format_registry::global().unpack(source, name)) << name;
        EXPECT_TRUE(accepted_prefix == source.size() || name == std::string_view{"pax.tar"})
            << name << " accepted prefix " << accepted_prefix;
    }
}

TEST(FormatRegistry, ReportsTheNestedContainerDepthLimit) {
    auto* tar = codec("tar");
    ASSERT_NE(tar, nullptr);
    auto nested = fixture("stored.zip");
    auto wrapper =
        liquid::format_tree{.codec = "tar", .tier = liquid::format_tier::canonical_round_trip};
    wrapper.entries.push_back(file_entry("nested.zip", nested));
    auto archive = tar->repack(wrapper);
    ASSERT_TRUE(archive) << archive.error().message;
    auto unpacked = liquid::format_registry::global().unpack(
        *archive,
        "wrapper.tar",
        liquid::format_limits{.maximum_depth = 1, .byte_budget = 1'000'000});
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    EXPECT_TRUE(unpacked->truncated);
    ASSERT_FALSE(unpacked->limits.empty());
    EXPECT_NE(unpacked->limits.front().find("depth limit"), std::string::npos);
}

TEST(FormatRegistry, ReportsTheDecodedByteBudget) {
    auto unpacked = liquid::format_registry::global().unpack(
        fixture("archive-a.zip"),
        "archive-a.zip",
        liquid::format_limits{.maximum_depth = 8, .byte_budget = 25});
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    EXPECT_TRUE(unpacked->truncated);
    ASSERT_FALSE(unpacked->limits.empty());
    EXPECT_NE(unpacked->limits.front().find("byte budget"), std::string::npos);
}

} // namespace
