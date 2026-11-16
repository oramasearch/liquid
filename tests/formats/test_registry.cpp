// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <format>
#include <fstream>
#include <initializer_list>
#include <iterator>
#include <limits>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <zlib.h>

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

// What `parquet_page_fixture` declares about its data page and footer. The defaults describe the
// payload honestly; a test overrides them to declare sizes or counts the payload does not have.
struct parquet_fixture_options {
    std::uint64_t values = 1;
    // The decompressed size both pages declare.
    std::optional<std::uint64_t> uncompressed;
    std::uint8_t codec = 0;
    // Thrift fields appended to FileMetaData after the row groups, starting at field 5.
    std::vector<std::byte> footer_fields;
};

// A one-row, uncompressed INT32 column with a dictionary and a caller-controlled data page.
auto parquet_page_fixture(std::span<const std::byte> payload,
                          std::uint8_t encoding,
                          std::uint8_t repetition = 0,
                          const parquet_fixture_options& options = {}) -> std::vector<std::byte> {
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
    append({0x15, 4, 0x15});
    varint(options.uncompressed.value_or(4) * 2);
    append({0x15, 8, 0x4c, 0x15, 2, 0x15, 0, 0, 0, 0, 0, 0, 0});
    const auto data_offset = bytes.size();
    append({0x15, 0, 0x15});
    varint(options.uncompressed.value_or(payload.size()) * 2);
    append({0x15});
    varint(payload.size() * 2);
    append({0x2c, 0x15});
    varint(options.values * 2);
    append({0x15, static_cast<std::uint8_t>(encoding * 2), 0x15, 6, 0x15, 6, 0, 0});
    bytes.insert(bytes.end(), payload.begin(), payload.end());
    const auto footer_offset = bytes.size();
    // FileMetaData: version, schema (root + leaf), row count and one row group.
    append({0x15,
            2,
            0x19,
            0x2c,
            0x48,
            1,
            'r',
            0x15,
            2,
            0,
            0x15,
            2,
            0x25,
            static_cast<std::uint8_t>(repetition * 2),
            0x18,
            1,
            'x',
            0,
            0x16,
            2,
            0x19,
            0x1c,
            0x19,
            0x1c,
            0x3c,
            0x15,
            2,
            0x29,
            0x18,
            1,
            'x',
            0x15,
            static_cast<std::uint8_t>(options.codec * 2),
            0x26});
    varint((payload.size() + 4) * 2);
    append({0x36});
    varint(data_offset * 2);
    append({0x26, 8, 0, 0, 0x26, 2, 0});
    bytes.insert(bytes.end(), options.footer_fields.begin(), options.footer_fields.end());
    append({0});
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

auto text(std::string_view value) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(value.size());
    std::ranges::transform(
        value, output.begin(), [](char character) { return static_cast<std::byte>(character); });
    return output;
}

auto crc32(std::span<const std::byte> input) -> std::uint32_t {
    auto crc = std::uint32_t{0xffffffffU};
    for (const auto byte : input) {
        crc ^= std::to_integer<std::uint32_t>(byte);
        for (auto bit = 0; bit < 8; ++bit) {
            crc = (crc >> 1U) ^ (0xedb88320U & (std::uint32_t{0} - (crc & 1U)));
        }
    }
    return ~crc;
}

void append_le(std::vector<std::byte>& output, std::uint64_t value, std::size_t width) {
    for (auto index = std::size_t{}; index < width; ++index) {
        output.push_back(static_cast<std::byte>((value >> (8U * index)) & 0xffU));
    }
}

using named_bytes = std::vector<std::pair<std::string, std::vector<std::byte>>>;

// The nested-archive builders below write the containers directly rather than through libliquid's
// private writers, so these tests use only the public format API.

// A gzip member of stored deflate blocks.
auto stored_gzip(std::span<const std::byte> input) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{std::byte{0x1f}, std::byte{0x8b}, std::byte{8}};
    output.resize(10);
    auto offset = std::size_t{};
    do {
        const auto size = std::min<std::size_t>(input.size() - offset, 0xffff);
        output.push_back(static_cast<std::byte>(offset + size == input.size() ? 1 : 0));
        append_le(output, size, 2);
        append_le(output, ~size & 0xffffU, 2);
        const auto block = input.subspan(offset, size);
        output.insert(output.end(), block.begin(), block.end());
        offset += size;
    } while (offset < input.size());
    append_le(output, crc32(input), 4);
    append_le(output, input.size(), 4);
    return output;
}

auto ar_archive(const named_bytes& members) -> std::vector<std::byte> {
    auto output = text("!<arch>\n");
    for (const auto& [name, bytes] : members) {
        const auto header = text(std::format(
            "{:<16}{:<12}{:<6}{:<6}{:<8}{:<10}`\n", name + "/", 0, 0, 0, 100644, bytes.size()));
        output.insert(output.end(), header.begin(), header.end());
        output.insert(output.end(), bytes.begin(), bytes.end());
        if (bytes.size() % 2 != 0) {
            output.push_back(std::byte{'\n'});
        }
    }
    return output;
}

// A ZIP archive of stored members.
auto stored_zip(const named_bytes& members) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    auto directory = std::vector<std::byte>{};
    for (const auto& [name, bytes] : members) {
        const auto offset = output.size();
        const auto checksum = crc32(bytes);
        const auto encoded_name = text(name);
        append_le(output, 0x04034b50U, 4);
        append_le(output, 20, 2);
        append_le(output, 0, 8);
        append_le(output, checksum, 4);
        append_le(output, bytes.size(), 4);
        append_le(output, bytes.size(), 4);
        append_le(output, name.size(), 2);
        append_le(output, 0, 2);
        output.insert(output.end(), encoded_name.begin(), encoded_name.end());
        output.insert(output.end(), bytes.begin(), bytes.end());
        append_le(directory, 0x02014b50U, 4);
        append_le(directory, 20, 2);
        append_le(directory, 20, 2);
        append_le(directory, 0, 8);
        append_le(directory, checksum, 4);
        append_le(directory, bytes.size(), 4);
        append_le(directory, bytes.size(), 4);
        append_le(directory, name.size(), 2);
        append_le(directory, 0, 12);
        append_le(directory, offset, 4);
        directory.insert(directory.end(), encoded_name.begin(), encoded_name.end());
    }
    const auto directory_offset = output.size();
    output.insert(output.end(), directory.begin(), directory.end());
    append_le(output, 0x06054b50U, 4);
    append_le(output, 0, 4);
    append_le(output, members.size(), 2);
    append_le(output, members.size(), 2);
    append_le(output, directory.size(), 4);
    append_le(output, directory_offset, 4);
    append_le(output, 0, 2);
    return output;
}

auto tar_archive(const named_bytes& members) -> std::vector<std::byte> {
    const auto* tar = codec("tar");
    if (!tar) {
        ADD_FAILURE() << "tar codec is absent";
        return {};
    }
    auto tree =
        liquid::format_tree{.codec = "tar", .tier = liquid::format_tier::canonical_round_trip};
    for (const auto& [name, bytes] : members) {
        tree.entries.push_back(file_entry(name, bytes));
    }
    auto archive = tar->repack(tree);
    EXPECT_TRUE(archive) << archive.error().message;
    return archive.value_or(std::vector<std::byte>{});
}

// A gzip-compressed .deb from its control.tar bytes and its payload files.
auto deb_package(std::span<const std::byte> control_tar, const named_bytes& payload)
    -> std::vector<std::byte> {
    return ar_archive({{"debian-binary", text("2.0\n")},
                       {"control.tar.gz", stored_gzip(control_tar)},
                       {"data.tar.gz", stored_gzip(tar_archive(payload))}});
}

auto deb_control() -> std::vector<std::byte> {
    return tar_archive({{"control", text("Package: nested\nVersion: 1\n")}});
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

TEST(FormatRegistry, ChargesPdfStreamsAgainstOneBudget) {
    // Eight FlateDecode streams, each inflating to about 16 KB of shown text. Each fits a 64 KiB
    // budget on its own; the text they leave behind does not.
    const auto content = "(" + std::string(16'000, 'a') + ") Tj";
    auto deflated = std::vector<Bytef>(compressBound(static_cast<uLong>(content.size())));
    auto deflated_size = static_cast<uLongf>(deflated.size());
    ASSERT_EQ(compress2(deflated.data(),
                        &deflated_size,
                        reinterpret_cast<const Bytef*>(content.data()),
                        static_cast<uLong>(content.size()),
                        Z_BEST_COMPRESSION),
              Z_OK);
    auto source_text = std::string{"%PDF-1.4\n"};
    for (auto object = 1; object <= 8; ++object) {
        source_text += std::to_string(object) + " 0 obj\n<< /Filter /FlateDecode >>\nstream\n";
        source_text.append(reinterpret_cast<const char*>(deflated.data()), deflated_size);
        source_text += "\nendstream\nendobj\n";
    }
    source_text += "%%EOF\n";
    const auto source = std::vector<std::byte>{
        reinterpret_cast<const std::byte*>(source_text.data()),
        reinterpret_cast<const std::byte*>(source_text.data() + source_text.size())};
    const auto* pdf = codec("pdf");
    ASSERT_NE(pdf, nullptr);
    auto over = pdf->unpack(source, liquid::format_limits{.byte_budget = 64 * 1024});
    ASSERT_FALSE(over);
    EXPECT_EQ(over.error().code, liquid::errc::unsupported);
    auto within = pdf->unpack(source);
    ASSERT_TRUE(within) << within.error().message;
    for (auto object = 1; object <= 8; ++object) {
        const auto* text = entry(*within, "text/" + std::to_string(object) + " 0");
        ASSERT_NE(text, nullptr) << object;
        EXPECT_EQ(text->bytes.size(), 16'001);
    }
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

// One chunk of four bytes, with identity shuffle filters so every unmasked filter is decoded.
// Both pipeline encodings share the 32-bit per-chunk filter mask (liquid #486).
TEST(FormatRegistry, BoundsHdf5FilterPipelinesToTheChunkMask) {
    const auto file =
        [](std::uint8_t version, std::uint8_t count, bool chunked, std::uint32_t mask) {
            auto output = std::vector<std::byte>(4204);
            auto position = std::size_t{};
            const auto put = [&](std::uint64_t value, std::size_t width) {
                for (auto index = std::size_t{}; index < width; ++index) {
                    output.at(position++) = static_cast<std::byte>((value >> (index * 8U)) & 0xffU);
                }
            };
            const auto put_bytes = [&](std::initializer_list<int> values) {
                for (const auto value : values) {
                    put(static_cast<std::uint64_t>(value), 1);
                }
            };
            const auto message = [&](std::uint8_t type, std::size_t size) {
                put(type, 1);
                put(size, 2);
                put(0, 1);
            };
            put_bytes({0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n', 2, 8, 8, 0});
            put(0, 8);
            put(~std::uint64_t{}, 8);
            put(output.size(), 8);
            put(48, 8);
            put(0, 4);
            put_bytes({'O', 'H', 'D', 'R', 2, 0, 16});
            message(6, 12);
            put_bytes({1, 0, 1, 'd'});
            put(128, 8);

            position = 128;
            put_bytes({'O', 'H', 'D', 'R', 2, 1});
            const auto chunk_size_position = position;
            put(0, 2);
            const auto messages_start = position;
            message(1, 12);
            put_bytes({2, 1, 0, 1});
            put(1, 8);
            message(3, 12);
            put_bytes({0x10, 0, 0, 0, 4, 0, 0, 0, 0, 0, 32, 0});
            message(8, chunked ? 19 : 8);
            if (chunked) {
                put_bytes({3, 2, 2});
                put(4096, 8);
                put(1, 4);
                put(4, 4);
            } else {
                put_bytes({3, 0, 4, 0});
                put(0x12345678, 4);
            }
            message(11, (version == 1 ? 8 : 2) + count * (version == 1 ? 8 : 6));
            put(version, 1);
            put(count, 1);
            if (version == 1) {
                put(0, 6);
            }
            for (auto index = std::size_t{}; index < count; ++index) {
                put(2, 2);
                if (version == 1) {
                    put(0, 2);
                }
                put(0, 4);
            }
            const auto chunk_size = position - messages_start;
            position = chunk_size_position;
            put(chunk_size, 2);

            position = 4096;
            put_bytes({'T', 'R', 'E', 'E', 1, 0, 1, 0});
            put(~std::uint64_t{}, 8);
            put(~std::uint64_t{}, 8);
            put(4, 4);
            put(mask, 4);
            put(0, 8);
            put(0, 8);
            put(4200, 8);
            position = 4200;
            put(0x12345678, 4);
            return output;
        };

    for (const auto version : {1, 2}) {
        for (const auto chunked : {true, false}) {
            for (const auto count : {0, 1, 31, 32, 33, 64, 255}) {
                for (const auto mask :
                     {std::uint32_t{}, std::uint32_t{1} << 31, ~std::uint32_t{}}) {
                    SCOPED_TRACE(::testing::Message()
                                 << "version=" << version << " count=" << count
                                 << " chunked=" << chunked << " mask=" << mask);
                    auto tree = liquid::format_registry::global().unpack(
                        file(version, count, chunked, mask), "filters.h5");
                    if (count > 32) {
                        ASSERT_FALSE(tree);
                        EXPECT_EQ(tree.error().code, liquid::errc::corrupt);
                        EXPECT_TRUE(tree.error().message.starts_with(
                            "HDF5 filter pipeline exceeds 32 filters"))
                            << tree.error().message;
                    } else {
                        ASSERT_TRUE(tree) << tree.error().message;
                        const auto* decoded = entry(*tree, "d");
                        ASSERT_NE(decoded, nullptr);
                        EXPECT_TRUE(decoded->opaque_reason.empty()) << decoded->opaque_reason;
                        EXPECT_EQ(decoded->bytes,
                                  (std::vector<std::byte>{std::byte{0x78},
                                                          std::byte{0x56},
                                                          std::byte{0x34},
                                                          std::byte{0x12}}));
                    }
                }
            }
        }
    }
}

// A root group whose one link message has an 8-byte name length (flags 0x03), so a length near
// 2^64 would wrap a position-plus-length bound check (liquid #477).
TEST(FormatRegistry, RefusesAnHdf5LinkNameLengthThatWrapsPastTheMessage) {
    const auto file = [](std::uint64_t name_size) {
        auto output = std::vector<std::byte>{};
        const auto put = [&](std::uint64_t value, std::size_t width) {
            for (auto index = std::size_t{}; index < width; ++index) {
                output.push_back(static_cast<std::byte>((value >> (index * 8U)) & 0xffU));
            }
        };
        const auto put_bytes = [&](std::initializer_list<int> values) {
            for (const auto value : values) {
                put(static_cast<std::uint64_t>(value), 1);
            }
        };
        put_bytes({0x89, 'H', 'D', 'F', '\r', '\n', 0x1a, '\n', 2, 8, 8, 0});
        constexpr auto root = std::uint64_t{48};
        constexpr auto message_size = std::uint64_t{2 + 8 + 4 + 8};
        constexpr auto end = root + 7 + 4 + message_size + 4;
        put(0, 8);
        put(~std::uint64_t{}, 8);
        put(end, 8);
        put(root, 8);
        put(0, 4);
        put_bytes({'O', 'H', 'D', 'R', 2, 0});
        put(4 + message_size, 1);
        put(6, 1);
        put(message_size, 2);
        put(0, 1);
        put(1, 1);
        put(0x03, 1);
        put(name_size, 8);
        put_bytes({'a', 'b', 'c', 'd'});
        put(root, 8);
        put(0, 4);
        return output;
    };

    auto honest = liquid::format_registry::global().unpack(file(4), "link.h5");
    ASSERT_TRUE(honest) << honest.error().message << " " << honest.error().context;
    EXPECT_EQ(honest->codec, "hdf5");

    // The name length is read at message offset 2, so the position after it is 10 and this length
    // makes position + length equal 1.
    const auto wrapping = ~std::uint64_t{} - 10 + 2;
    for (const auto name_size : {wrapping, ~std::uint64_t{}, std::uint64_t{13}}) {
        auto crafted = liquid::format_registry::global().unpack(file(name_size), "link.h5");
        ASSERT_FALSE(crafted) << name_size;
        EXPECT_EQ(crafted.error().code, liquid::errc::corrupt) << name_size;
        EXPECT_NE(crafted.error().message.find("link name"), std::string::npos)
            << name_size << ": " << crafted.error().message;
    }
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

auto budget_reached(const liquid::format_tree& tree) -> bool {
    return tree.truncated && std::ranges::any_of(tree.limits, [](const std::string& limit) {
               return limit.starts_with("Parquet byte budget reached");
           });
}

TEST(FormatRegistry, ChargesParquetDeclaredPageSizeBeforeAllocating) {
    // A 16-byte page that declares 64 MiB once decompressed, under a 1 MiB budget.
    const auto payload = std::vector<std::byte>(16, std::byte{0x5a});
    for (const auto codec : {std::uint8_t{2}, std::uint8_t{6}}) {
        SCOPED_TRACE(codec);
        auto tree = liquid::format_registry::global().unpack(
            parquet_page_fixture(
                payload, 0, 0, {.uncompressed = std::uint64_t{64} << 20U, .codec = codec}),
            "page.parquet",
            liquid::format_limits{.byte_budget = std::uint64_t{1} << 20U});
        ASSERT_TRUE(tree) << tree.error().message;
        EXPECT_TRUE(budget_reached(*tree));
        EXPECT_EQ(entry(*tree, "row-group=0/x"), nullptr);
    }
}

TEST(FormatRegistry, ChargesParquetDeclaredValueCountBeforeDecoding) {
    // A five-byte page declaring a million values: one RLE run of dictionary index 5, which the
    // one-value dictionary does not have. The levels alone would take 16 MB.
    constexpr auto values = std::uint64_t{1'000'000};
    auto payload = parquet_run(values << 1U, 1);
    payload.back() = std::byte{5};
    payload.insert(payload.begin(), std::byte{1});
    auto tree = liquid::format_registry::global().unpack(
        parquet_page_fixture(payload, 8, 0, {.values = values}),
        "count.parquet",
        liquid::format_limits{.byte_budget = std::uint64_t{1} << 20U});
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_TRUE(budget_reached(*tree));
    EXPECT_EQ(entry(*tree, "row-group=0/x"), nullptr);
}

TEST(FormatRegistry, ChargesParquetFooterCollectionsBeforeReserving) {
    const auto valid = [](std::uint8_t type, std::size_t count) {
        // Field 5 of FileMetaData, a list (type 9) of i8 or a map (type 11) of i8 to i8.
        auto fields = std::vector<std::byte>{static_cast<std::byte>(0x10U | type)};
        if (type == 9) {
            fields.push_back(std::byte{0xf3});
        }
        auto size = parquet_run(count);
        fields.insert(fields.end(), size.begin(), size.end());
        if (type == 11) {
            fields.push_back(std::byte{0x33});
        }
        fields.resize(fields.size() + (type == 11 ? 2 : 1) * count, std::byte{1});
        return fields;
    };
    const auto payload = parquet_run(2, 1);
    for (const auto type : {std::uint8_t{9}, std::uint8_t{11}}) {
        SCOPED_TRACE(type);
        // 200,000 elements fit in the footer but not in a 1 MiB budget once each is a value.
        auto over = liquid::format_registry::global().unpack(
            parquet_page_fixture(payload, 3, 0, {.footer_fields = valid(type, 200'000)}),
            "footer.parquet",
            liquid::format_limits{.byte_budget = std::uint64_t{1} << 20U});
        ASSERT_FALSE(over);
        EXPECT_EQ(over.error().code, liquid::errc::unsupported);
        EXPECT_TRUE(over.error().message.starts_with("Parquet byte budget reached"))
            << over.error().message;
        auto within = liquid::format_registry::global().unpack(
            parquet_page_fixture(payload, 3, 0, {.footer_fields = valid(type, 1'000)}),
            "footer.parquet");
        ASSERT_TRUE(within) << within.error().message;
        EXPECT_NE(entry(*within, "row-group=0/x"), nullptr);
    }
    // A map declaring 2^40 entries in a few bytes is rejected before anything is reserved.
    auto map = std::vector<std::byte>{std::byte{0x1b}};
    auto size = parquet_run(std::uint64_t{1} << 40U);
    map.insert(map.end(), size.begin(), size.end());
    map.push_back(std::byte{0x33});
    auto lying = liquid::format_registry::global().unpack(
        parquet_page_fixture(payload, 3, 0, {.footer_fields = map}), "map.parquet");
    ASSERT_FALSE(lying);
    EXPECT_EQ(lying.error().code, liquid::errc::corrupt);
}

TEST(FormatRegistry, ChargesParquetColumnsAgainstOneBudget) {
    auto source = fixture("sample.parquet");
    const auto* parquet = codec("parquet");
    ASSERT_NE(parquet, nullptr);
    auto whole = parquet->unpack(source);
    ASSERT_TRUE(whole) << whole.error().message;
    ASSERT_FALSE(whole->truncated);
    auto total = std::uint64_t{};
    auto largest = std::uint64_t{};
    for (const auto& item : whole->entries) {
        total += item.bytes.size();
        largest = std::max<std::uint64_t>(largest, item.bytes.size());
    }
    ASSERT_GT(total, largest);
    // The smallest budget for which `fits` holds; unpacking is monotonic in its budget.
    const auto smallest = [&](auto fits) {
        auto low = std::uint64_t{1};
        auto high = std::uint64_t{256} << 20U;
        while (low < high) {
            const auto middle = low + (high - low) / 2;
            if (fits(parquet->unpack(source, liquid::format_limits{.byte_budget = middle}))) {
                high = middle;
            } else {
                low = middle + 1;
            }
        }
        return low;
    };
    const auto opens = smallest([](const auto& tree) { return tree.has_value(); });
    const auto complete = smallest([](const auto& tree) { return tree && !tree->truncated; });
    // Each column's bytes are charged on top of everything before it, so the budget that decodes
    // the whole file exceeds the one that only reads its footer by at least every column.
    EXPECT_GE(complete - opens, total);
    auto short_by_one = parquet->unpack(source, liquid::format_limits{.byte_budget = complete - 1});
    ASSERT_TRUE(short_by_one) << short_by_one.error().message;
    EXPECT_TRUE(budget_reached(*short_by_one));
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

// liquid#438: each package's data.tar holds the next package, and the innermost is not a valid
// package. The package codec decodes its own members through the registry, and that call must stay
// inside the outermost depth limit instead of reaching the innermost level.
TEST(FormatRegistry, NestedPackagesStayInsideTheOutermostDepthLimit) {
    auto package = text("!<arch>\n");
    for (auto level = 0; level < 6; ++level) {
        package = deb_package(deb_control(), {{"inner.deb", package}});
    }
    auto unpacked = liquid::format_registry::global().unpack(
        package, "nested.deb", liquid::format_limits{.maximum_depth = 2, .byte_budget = 1'000'000});
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    const auto* inner = entry(*unpacked, "data/inner.deb");
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(inner->nested, nullptr);
    EXPECT_TRUE(inner->nested->truncated);
    const auto* innermost = entry(*inner->nested, "data/inner.deb");
    ASSERT_NE(innermost, nullptr);
    EXPECT_EQ(innermost->opaque_reason, "depth limit reached");
    EXPECT_EQ(innermost->nested, nullptr);
}

// The same for documents: the document codec decodes its ZIP container through the registry.
TEST(FormatRegistry, NestedDocumentsStayInsideTheOutermostDepthLimit) {
    auto document = text("PK\x03\x04 is not a document");
    for (auto level = 0; level < 6; ++level) {
        document = stored_zip({{"inner.docx", document}});
    }
    auto unpacked = liquid::format_registry::global().unpack(
        document,
        "nested.docx",
        liquid::format_limits{.maximum_depth = 2, .byte_budget = 1'000'000});
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    EXPECT_EQ(unpacked->codec, "ooxml");
    const auto* inner = entry(*unpacked, "inner.docx");
    ASSERT_NE(inner, nullptr);
    ASSERT_NE(inner->nested, nullptr);
    const auto* innermost = entry(*inner->nested, "inner.docx");
    ASSERT_NE(innermost, nullptr);
    EXPECT_EQ(innermost->opaque_reason, "depth limit reached");
    EXPECT_EQ(innermost->nested, nullptr);
}

// The wrapper's members, decoded in path order, spend all but its tar headers of the budget, which
// is too little for the package's own members. The package decodes against what is left, not
// against a new budget.
TEST(FormatRegistry, NestedPackagesDrawOnTheRemainingByteBudget) {
    const auto package =
        deb_package(deb_control(), {{"payload.bin", std::vector<std::byte>(4096, std::byte{'x'})}});
    const auto filler = std::vector<std::byte>(8192, std::byte{'f'});
    const auto archive = tar_archive({{"filler.bin", filler}, {"package.deb", package}});
    auto unpacked = liquid::format_registry::global().unpack(
        archive,
        "wrapper.tar",
        liquid::format_limits{.maximum_depth = 8, .byte_budget = archive.size()});
    ASSERT_TRUE(unpacked) << unpacked.error().message;
    EXPECT_TRUE(unpacked->truncated);
    const auto* nested = entry(*unpacked, "package.deb");
    ASSERT_NE(nested, nullptr);
    EXPECT_EQ(nested->nested, nullptr);
    EXPECT_FALSE(nested->opaque_reason.empty());
    EXPECT_EQ(nested->bytes, package);
}

// The registry picks codecs by content, so a control.tar may itself be a package. Decoding a
// codec's members may not decode members in turn, which would otherwise recurse without bound.
TEST(FormatRegistry, RefusesPackageMembersThatDecodeMembersThemselves) {
    const auto inner = deb_package(deb_control(), {{"file.txt", text("inner\n")}});
    const auto package = deb_package(inner, {{"file.txt", text("outer\n")}});
    auto unpacked = liquid::format_registry::global().unpack(package, "outer.deb");
    ASSERT_FALSE(unpacked);
    EXPECT_TRUE(unpacked.error().message.starts_with("format members nest inside format members"))
        << unpacked.error().message;
}

auto append_be32(std::vector<std::byte>& output, std::uint32_t value) -> void {
    for (auto shift : {24U, 16U, 8U, 0U}) {
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
    }
}

auto append_text(std::vector<std::byte>& output, std::string_view text) -> void {
    for (const auto character : text) {
        output.push_back(static_cast<std::byte>(character));
    }
}

struct rpm_string_entry {
    std::uint32_t tag;
    std::uint32_t offset;
};

// A minimal RPM: an empty signature header, a main header whose string entries point into `store`,
// and a zstd payload of one raw block (at most 255 bytes), defaulting to the cpio trailer.
auto rpm_package(std::span<const rpm_string_entry> entries,
                 std::span<const std::byte> store,
                 std::span<const std::byte> payload = {}) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>(96);
    output[0] = std::byte{0xed};
    output[1] = std::byte{0xab};
    output[2] = std::byte{0xee};
    output[3] = std::byte{0xdb};
    const auto header_start = [&](std::uint32_t count, std::uint32_t size) {
        append_be32(output, 0x8eade801U);
        append_be32(output, 0);
        append_be32(output, count);
        append_be32(output, size);
    };
    header_start(0, 0);
    header_start(static_cast<std::uint32_t>(entries.size()),
                 static_cast<std::uint32_t>(store.size()));
    for (const auto& entry : entries) {
        append_be32(output, entry.tag);
        append_be32(output, 6);
        append_be32(output, entry.offset);
        append_be32(output, 1);
    }
    output.insert(output.end(), store.begin(), store.end());
    auto trailer = std::vector<std::byte>{};
    append_text(trailer, "070701");
    for (auto field = 0; field < 13; ++field) {
        append_text(trailer, field == 11 ? "0000000b" : "00000000");
    }
    append_text(trailer, "TRAILER!!!");
    trailer.resize(124);
    if (payload.empty()) {
        payload = trailer;
    }
    EXPECT_LE(payload.size(), 255);
    for (const auto value : {0x28U, 0xb5U, 0x2fU, 0xfdU, 0x20U}) {
        output.push_back(static_cast<std::byte>(value));
    }
    output.push_back(static_cast<std::byte>(payload.size()));
    const auto block = 1U | (static_cast<std::uint32_t>(payload.size()) << 3U);
    for (auto shift : {0U, 8U, 16U}) {
        output.push_back(static_cast<std::byte>((block >> shift) & 0xffU));
    }
    output.insert(output.end(), payload.begin(), payload.end());
    return output;
}

auto entry_text(const liquid::format_tree& tree, std::string_view path) -> std::string {
    const auto* found = entry(tree, path);
    return found == nullptr ? std::string{}
                            : std::string{reinterpret_cast<const char*>(found->bytes.data()),
                                          found->bytes.size()};
}

TEST(FormatRegistry, ReadsRpmNameAndVersionFromTheMainHeader) {
    auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    auto store = std::vector<std::byte>{};
    append_text(store,
                std::string_view{"demo\0"
                                 "1.0\0",
                                 9});
    const auto entries = std::array{rpm_string_entry{1000, 0}, rpm_string_entry{1001, 5}};
    auto tree = rpm->unpack(rpm_package(entries, store));
    ASSERT_TRUE(tree) << tree.error().message;
    EXPECT_EQ(entry_text(*tree, ".metadata"), "Name: demo\nVersion: 1.0\n");
}

// liquid #473: many index entries naming one long string must not each copy it.
TEST(FormatRegistry, ChargesRpmHeaderStringCopiesToTheByteBudget) {
    auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    const auto limits = liquid::format_limits{.byte_budget = 1024ULL * 1024ULL};

    // The reported shape: distinct tags, every one at offset 0 of a string with no terminator.
    // Uncharged, the 64 copies come to 16 MiB; the six tags the reader keeps still need 1.5 MiB.
    const auto distinct_store = std::vector<std::byte>(256U * 1024U, std::byte{'a'});
    auto distinct = std::vector<rpm_string_entry>{};
    for (auto tag = 1000U; tag < 1064U; ++tag) {
        distinct.push_back({tag, 0});
    }
    auto refused = rpm->unpack(rpm_package(distinct, distinct_store), limits);
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unsupported);
    EXPECT_NE(refused.error().message.find("byte budget"), std::string::npos);

    // One kept tag repeated: each 64 KiB copy fits on its own, 64 of them do not.
    const auto repeated_store = std::vector<std::byte>(64U * 1024U, std::byte{'a'});
    const auto repeated = std::vector<rpm_string_entry>(64, rpm_string_entry{1000, 0});
    auto repeated_refused = rpm->unpack(rpm_package(repeated, repeated_store), limits);
    ASSERT_FALSE(repeated_refused);
    EXPECT_EQ(repeated_refused.error().code, liquid::errc::unsupported);

    // Tags the reader does not keep cost nothing, however many name the string.
    auto ignored = std::vector<rpm_string_entry>{};
    for (auto tag = 2000U; tag < 2064U; ++tag) {
        ignored.push_back({tag, 0});
    }
    EXPECT_TRUE(rpm->unpack(rpm_package(ignored, distinct_store), limits));
}

// A newc header and its NUL-terminated name, deliberately without padding or file bytes.
auto cpio_member(std::string_view name, std::uint32_t size) -> std::vector<std::byte> {
    auto output = std::vector<std::byte>{};
    append_text(output, "070701");
    auto fields = std::array<std::uint32_t, 13>{};
    fields[1] = 0100644;
    fields[6] = size;
    fields[11] = static_cast<std::uint32_t>(name.size() + 1);
    constexpr auto hex = std::string_view{"0123456789abcdef"};
    for (const auto value : fields) {
        for (auto digit = 8U; digit > 0; --digit) {
            output.push_back(static_cast<std::byte>(hex[(value >> ((digit - 1) * 4U)) & 15U]));
        }
    }
    append_text(output, name);
    output.push_back(std::byte{});
    return output;
}

TEST(FormatRegistry, RejectsRpmTruncatedSignaturePadding) {
    const auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    for (auto size = 1U; size < 8; ++size) {
        for (auto available = 0U; available < 8 - size; ++available) {
            SCOPED_TRACE("size=" + std::to_string(size) + " padding=" + std::to_string(available));
            auto blob = rpm_package({}, {});
            blob[111] = static_cast<std::byte>(size);
            blob.resize(112 + size + available);
            const auto refused = rpm->unpack(blob);
            ASSERT_FALSE(refused);
            EXPECT_EQ(refused.error().code, liquid::errc::corrupt);
            EXPECT_EQ(refused.error().message, "truncated rpm signature padding (rpm)");
        }
    }
}

TEST(FormatRegistry, RejectsRpmCpioTruncatedNamePadding) {
    const auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    for (const auto name : {"TRAILER!!!", "", "xx", "xxx", "xxxx"}) {
        for (const auto size : {0U, 1U, 0xffffffffU}) {
            const auto member = cpio_member(name, size);
            const auto padding = (4 - member.size() % 4) % 4;
            for (auto available = std::size_t{}; available < padding; ++available) {
                SCOPED_TRACE(std::string{name} + " size=" + std::to_string(size) +
                             " padding=" + std::to_string(available));
                auto payload = member;
                payload.resize(payload.size() + available);
                const auto refused = rpm->unpack(rpm_package({}, {}, payload));
                ASSERT_FALSE(refused);
                EXPECT_EQ(refused.error().code, liquid::errc::corrupt);
                EXPECT_EQ(refused.error().message, "truncated cpio name padding (rpm)");
            }
        }
    }
}

TEST(FormatRegistry, RejectsRpmCpioTruncatedFilePadding) {
    const auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    for (const auto name : {"x", "TRAILER!!!"}) {
        for (const auto size : {1U, 2U, 3U}) {
            auto member = cpio_member(name, size);
            member.resize(member.size() + (4 - member.size() % 4) % 4);
            member.resize(member.size() + size, std::byte{'x'});
            const auto padding = (4 - member.size() % 4) % 4;
            for (auto available = std::size_t{}; available < padding; ++available) {
                SCOPED_TRACE(std::string{name} + " size=" + std::to_string(size) +
                             " padding=" + std::to_string(available));
                auto payload = member;
                payload.resize(payload.size() + available);
                const auto refused = rpm->unpack(rpm_package({}, {}, payload));
                ASSERT_FALSE(refused);
                EXPECT_EQ(refused.error().code, liquid::errc::corrupt);
                EXPECT_EQ(refused.error().message, "truncated cpio file padding (rpm)");
            }
        }
    }
}

TEST(FormatRegistry, RejectsRpmCpioTruncatedFileBytes) {
    const auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    for (const auto size : {1U, 2U, 3U, 4U, 0xffffffffU}) {
        for (auto available = 0U; available < std::min(size, 4U); ++available) {
            SCOPED_TRACE("size=" + std::to_string(size) + " bytes=" + std::to_string(available));
            auto payload = cpio_member("x", size);
            payload.resize(payload.size() + available);
            const auto refused = rpm->unpack(rpm_package({}, {}, payload));
            ASSERT_FALSE(refused);
            EXPECT_EQ(refused.error().code, liquid::errc::corrupt);
            EXPECT_EQ(refused.error().message, "truncated cpio file (rpm)");
        }
    }
}

TEST(FormatRegistry, ReadsRpmCpioAlignedMembersAndTrailer) {
    const auto* rpm = codec("rpm");
    ASSERT_NE(rpm, nullptr);
    for (const auto name : {"x", "xx", "xxx", "xxxx"}) {
        for (const auto size : {0U, 1U, 2U, 3U, 4U}) {
            SCOPED_TRACE(std::string{name} + " size=" + std::to_string(size));
            auto payload = cpio_member(name, size);
            payload.resize(payload.size() + (4 - payload.size() % 4) % 4);
            payload.resize(payload.size() + size, std::byte{'x'});
            payload.resize(payload.size() + (4 - payload.size() % 4) % 4);
            auto trailer = cpio_member("TRAILER!!!", 0);
            payload.insert(payload.end(), trailer.begin(), trailer.end());
            payload.resize(payload.size() + (4 - payload.size() % 4) % 4);
            const auto tree = rpm->unpack(rpm_package({}, {}, payload));
            ASSERT_TRUE(tree) << tree.error().message;
            const auto path = std::string{"data/"} + name;
            ASSERT_NE(entry(*tree, path), nullptr);
            EXPECT_EQ(entry_text(*tree, path), std::string(size, 'x'));
        }
    }
}

} // namespace
