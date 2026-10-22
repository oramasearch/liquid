#include <gtest/gtest.h>

#include <algorithm>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "formats/json.hpp"
#include "formats/structural_budget.hpp"
#include "formats/xml/xml.hpp"
#include "liquid/format.hpp"

namespace {

using liquid::formats::structural_budget;
using bytes = std::vector<std::byte>;

auto byte(bytes& output, std::uint64_t value) -> void {
    output.push_back(static_cast<std::byte>(value));
}

auto variable(bytes& output, std::uint64_t value) -> void {
    while (value >= 128) {
        byte(output, (value & 127) | 128);
        value >>= 7;
    }
    byte(output, value);
}

auto field(bytes& output, std::uint8_t id, std::uint8_t type) -> void {
    byte(output, type);
    variable(output, id * 2U);
}

auto list_header(bytes& output, std::size_t count, std::uint8_t type) -> void {
    byte(output, (std::min(count, std::size_t{15}) << 4U) | type);
    if (count >= 15) {
        variable(output, count);
    }
}

// A valid metadata-only file; the optional field exercises unknown compact values too.
auto parquet(std::size_t groups, const bytes& extra = {}, const bytes& page = {}) -> bytes {
    auto footer = bytes{};
    field(footer, 1, 5);
    variable(footer, 2); // version 1, zigzag encoded
    field(footer, 2, 9);
    list_header(footer, groups + 2, 12);
    for (auto index = std::size_t{}; index < groups + 2; ++index) {
        field(footer, 4, 8);
        variable(footer, 1);
        byte(footer, 'x');
        if (index == groups + 1) {
            field(footer, 1, 5);
            variable(footer, 2); // INT32 leaf
        } else {
            field(footer, 5, 5);
            variable(footer, 2); // one child
        }
        byte(footer, 0);
    }
    field(footer, 4, 9);
    list_header(footer, page.empty() ? 0 : 1, 12);
    if (!page.empty()) {
        field(footer, 1, 9); // row group columns
        list_header(footer, 1, 12);
        field(footer, 3, 12); // column metadata
        field(footer, 3, 9);  // schema path
        list_header(footer, 1, 8);
        variable(footer, 1);
        byte(footer, 'x');
        for (const auto [id, value] : {std::pair{4, 0}, std::pair{6, 4}, std::pair{9, 4}}) {
            field(footer, id, 6);
            variable(footer, value * 2U);
        }
        byte(footer, 0);     // metadata end
        byte(footer, 0);     // column end
        field(footer, 3, 6); // row count
        variable(footer, 2);
        byte(footer, 0); // row group end
    }
    footer.insert(footer.end(), extra.begin(), extra.end());
    byte(footer, 0);
    auto output = bytes{std::byte{'P'}, std::byte{'A'}, std::byte{'R'}, std::byte{'1'}};
    output.insert(output.end(), page.begin(), page.end());
    output.insert(output.end(), footer.begin(), footer.end());
    for (auto shift = 0U; shift < 32; shift += 8) {
        byte(output, footer.size() >> shift);
    }
    output.insert(output.end(), {std::byte{'P'}, std::byte{'A'}, std::byte{'R'}, std::byte{'1'}});
    return output;
}

auto page(const bytes& extra = {}) -> bytes {
    auto output = bytes{};
    for (const auto [id, value] : {std::pair{1, 0}, std::pair{2, 4}, std::pair{3, 4}}) {
        field(output, id, 5);
        variable(output, value * 2U);
    }
    field(output, 5, 12);
    field(output, 1, 5);
    variable(output, 2); // one value
    field(output, 2, 5);
    variable(output, 0); // PLAIN
    byte(output, 0);
    output.insert(output.end(), extra.begin(), extra.end());
    byte(output, 0);
    output.insert(output.end(), {std::byte{42}, std::byte{0}, std::byte{0}, std::byte{0}});
    return output;
}

auto wide_field(std::size_t count) -> bytes {
    auto output = bytes{};
    field(output, 8, 9);
    list_header(output, count, 3);
    output.insert(output.end(), count, std::byte{0});
    return output;
}

auto codec_named(std::string_view name) -> const liquid::format_codec* {
    for (const auto* codec : liquid::format_registry::global().codecs()) {
        if (codec->descriptor().name == name) {
            return codec;
        }
    }
    return nullptr;
}

auto parquet_codec() -> const liquid::format_codec* {
    return codec_named("parquet");
}

auto little(bytes& output, std::uint64_t value, std::size_t width) -> void {
    for (auto index = std::size_t{}; index < width; ++index) {
        byte(output, (value >> (index * 8)) & 255U);
    }
}

auto big(bytes& output, std::size_t at, std::uint64_t value, std::size_t width) -> void {
    for (auto index = std::size_t{}; index < width; ++index) {
        output[at + index] = static_cast<std::byte>((value >> ((width - 1 - index) * 8)) & 255U);
    }
}

// A table b-tree whose interior pages hold no cells and chain through their right pointers.
auto sqlite(std::size_t interior) -> bytes {
    constexpr auto page_size = std::size_t{512};
    auto output = bytes((interior + 1) * page_size);
    const auto signature = std::string_view{"SQLite format 3"};
    std::ranges::transform(
        signature, output.begin(), [](char value) { return static_cast<std::byte>(value); });
    big(output, 16, page_size, 2);
    big(output, 28, interior + 1, 4);
    big(output, 56, 1, 4); // UTF-8
    for (auto number = std::size_t{1}; number <= interior + 1; ++number) {
        const auto header = (number - 1) * page_size + (number == 1 ? 100 : 0);
        output[header] = std::byte{number <= interior ? std::uint8_t{0x05} : std::uint8_t{0x0d}};
        if (number <= interior) {
            big(output, header + 8, number + 1, 4);
        }
    }
    return output;
}

auto object_header(const std::vector<std::pair<std::uint16_t, bytes>>& messages) -> bytes {
    auto body = bytes{};
    for (const auto& [type, data] : messages) {
        little(body, type, 2);
        little(body, data.size(), 2);
        little(body, 0, 4);
        body.insert(body.end(), data.begin(), data.end());
        body.resize((body.size() + 7) / 8 * 8);
    }
    auto output = bytes{std::byte{1}, std::byte{0}};
    little(output, messages.size(), 2);
    little(output, 1, 4);
    little(output, body.size(), 4);
    little(output, 0, 4);
    output.insert(output.end(), body.begin(), body.end());
    return output;
}

auto hard_link(std::uint64_t target) -> bytes {
    auto output = bytes{std::byte{1}, std::byte{0}, std::byte{1}, std::byte{'g'}};
    little(output, target, 8);
    return output;
}

// A level-1 B-tree node with at most one child; chaining these never repeats an address.
auto tree_node(std::uint8_t type, std::optional<std::uint64_t> child, std::size_t key_size)
    -> bytes {
    auto output = bytes{std::byte{'T'}, std::byte{'R'}, std::byte{'E'}, std::byte{'E'}};
    byte(output, type);
    byte(output, 1);
    little(output, child ? 1 : 0, 2);
    little(output, 0, 16);
    if (child) {
        little(output, 0, key_size);
        little(output, *child, 8);
    }
    return output;
}

// Objects are appended innermost first, so every address is known before it is referenced.
struct hdf5_file {
    bytes data = bytes(48);

    auto add(const bytes& value) -> std::uint64_t {
        const auto offset = data.size();
        data.insert(data.end(), value.begin(), value.end());
        return offset;
    }

    auto chain(std::size_t count, const auto& node) -> std::uint64_t {
        auto next = std::optional<std::uint64_t>{};
        for (auto index = std::size_t{}; index < count; ++index) {
            next = add(node(next));
        }
        return *next;
    }

    auto finish(std::uint64_t root) const -> bytes {
        auto output = bytes{std::byte{0x89},
                            std::byte{'H'},
                            std::byte{'D'},
                            std::byte{'F'},
                            std::byte{'\r'},
                            std::byte{'\n'},
                            std::byte{0x1a},
                            std::byte{'\n'},
                            std::byte{2},
                            std::byte{8},
                            std::byte{8},
                            std::byte{0}};
        little(output, 0, 8);           // base address
        little(output, ~0ULL, 8);       // no superblock extension
        little(output, data.size(), 8); // end of file
        little(output, root, 8);
        little(output, 0, 4);
        output.insert(output.end(), data.begin() + 48, data.end());
        return output;
    }
};

TEST(StructuralLimits, JsonCountsArraysObjectsAndMixedNesting) {
    for (const auto kind : {0, 1, 2}) {
        for (const auto depth :
             {structural_budget::maximum_depth - 1, structural_budget::maximum_depth}) {
            auto source = std::string{"0"};
            for (auto index = std::size_t{}; index < depth; ++index) {
                source = (kind == 1 || (kind == 2 && index % 2) ? "{\"a\":" + source + "}"
                                                                : "[" + source + "]");
            }
            const auto parsed = liquid::formats::json::parse(source);
            EXPECT_EQ(parsed.has_value(), depth < structural_budget::maximum_depth);
            if (!parsed) {
                EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos);
            }
        }
    }
}

TEST(StructuralLimits, JsonChargesWideArraysAndObjectKeysToOneBudget) {
    auto source = std::string{"["};
    for (auto index = std::size_t{}; index < structural_budget::maximum_work; ++index) {
        source += index ? ",0" : "0";
    }
    source += "]";
    const auto wide = liquid::formats::json::parse(source);
    ASSERT_FALSE(wide);
    EXPECT_NE(wide.error().message.find("structural limit"), std::string::npos);

    // Each member costs its key and its value, so this object exceeds the budget too.
    source = "{";
    for (auto index = std::size_t{}; index < structural_budget::maximum_work / 2; ++index) {
        source += (index ? ",\"" : "\"") + std::to_string(index) + "\":0";
    }
    source += "}";
    const auto members = liquid::formats::json::parse(source);
    ASSERT_FALSE(members);
    EXPECT_NE(members.error().message.find("structural limit"), std::string::npos);
    EXPECT_FALSE(liquid::formats::json::parse(R"({"a":0,"\u0061":1})"));
}

TEST(StructuralLimits, XmlCountsPairedAndSelfClosingElementsBeforeCopyingNamespaces) {
    for (const auto self_closing : {false, true}) {
        for (const auto depth :
             {structural_budget::maximum_depth, structural_budget::maximum_depth + 1}) {
            auto source = std::string{};
            for (auto index = std::size_t{1}; index < depth; ++index) {
                source += "<x xmlns:p=\"urn:test\">";
            }
            source += self_closing ? "<p:x/>" : "<p:x></p:x>";
            for (auto index = std::size_t{1}; index < depth; ++index) {
                source += "</x>";
            }
            const auto parsed = liquid::formats::xml::canonicalise(source);
            EXPECT_EQ(parsed.has_value(), depth <= structural_budget::maximum_depth);
            if (!parsed) {
                EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos);
            }
        }
    }
}

TEST(StructuralLimits, XmlChildrenDoNotCopyInheritedNamespaces) {
    // Copying every inherited declaration into each child costs declarations times children.
    constexpr auto count = std::size_t{50'000};
    auto source = std::string{"<x"};
    for (auto index = std::size_t{}; index < count; ++index) {
        source += " xmlns:p" + std::to_string(index) + "=\"urn:" + std::to_string(index) + "\"";
    }
    source += ">";
    for (auto index = std::size_t{}; index < count; ++index) {
        source += "<p7 xmlns:q=\"urn:q\"><q:y/></p7>";
    }
    source += "<p49999:z/></x>";
    const auto parsed = liquid::formats::xml::canonicalise(source);
    ASSERT_TRUE(parsed) << parsed.error().message;
    EXPECT_NE(parsed->find("<{urn:q}y/>"), std::string::npos);
    EXPECT_TRUE(parsed->ends_with("<{urn:49999}z/></x>"));
}

TEST(StructuralLimits, XmlSharesWorkAcrossSiblingElementsAndAttributes) {
    auto source = std::string{"<x>"};
    for (auto index = std::size_t{}; index < structural_budget::maximum_work / 2; ++index) {
        source += "<y a=\"b\"/>";
    }
    source += "</x>";
    const auto parsed = liquid::formats::xml::canonicalise(source);
    ASSERT_FALSE(parsed);
    EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos);
}

TEST(StructuralLimits, ParquetCountsListsSetsMapsStructsAndMixedContainers) {
    const auto* codec = parquet_codec();
    ASSERT_NE(codec, nullptr);
    for (const auto kind : {9, 10, 11, 12, 0}) {
        for (const auto levels : {62U, 64U}) {
            auto value = bytes{std::byte{0}};
            auto type = std::uint8_t{3};
            for (auto depth = std::size_t{}; depth < levels; ++depth) {
                auto outer = bytes{};
                const auto container = kind ? kind : 9 + depth % 4;
                if (container == 9 || container == 10) {
                    list_header(outer, 1, type);
                } else if (container == 11) {
                    variable(outer, 1);
                    byte(outer, depth % 2 ? (type << 4U) | 3U : (3U << 4U) | type);
                    if (depth % 2 == 0) {
                        byte(outer, 0);
                    }
                } else {
                    field(outer, 1, type);
                }
                outer.insert(outer.end(), value.begin(), value.end());
                if (container == 12 || (container == 11 && depth % 2)) {
                    byte(outer, 0);
                }
                value = std::move(outer);
                type = static_cast<std::uint8_t>(container);
            }
            auto extra = bytes{};
            field(extra, 5, type);
            extra.insert(extra.end(), value.begin(), value.end());
            const auto parsed = codec->unpack(parquet(0, extra), {});
            EXPECT_EQ(parsed.has_value(), levels == 62) << kind;
            if (!parsed) {
                EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos)
                    << kind;
            }
        }
    }
}

TEST(StructuralLimits, ParquetChecksFlatSchemaDepthSeparatelyFromCompactDepth) {
    const auto* codec = parquet_codec();
    ASSERT_NE(codec, nullptr);
    const auto shallow = codec->unpack(parquet(structural_budget::maximum_depth - 1), {});
    ASSERT_TRUE(shallow) << shallow.error().message;
    const auto deep = codec->unpack(parquet(structural_budget::maximum_depth), {});
    ASSERT_FALSE(deep);
    EXPECT_NE(deep.error().message.find("schema structural limit"), std::string::npos);
}

TEST(StructuralLimits, ParquetRejectsUnaffordableListAndMapReservations) {
    const auto* codec = parquet_codec();
    ASSERT_NE(codec, nullptr);
    for (const auto type : {9, 11}) {
        auto extra = bytes{};
        field(extra, 5, type);
        if (type == 9) {
            list_header(extra, structural_budget::maximum_work + 1, 3);
        } else {
            variable(extra, structural_budget::maximum_work / 2 + 1);
            byte(extra, 0x33);
        }
        const auto parsed = codec->unpack(parquet(0, extra), {});
        ASSERT_FALSE(parsed);
        EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos);
    }
}

TEST(StructuralLimits, ParquetSharesWorkBetweenFooterAndColumnPageHeaders) {
    const auto* codec = parquet_codec();
    ASSERT_NE(codec, nullptr);
    const auto footer = wide_field(200000);
    const auto header = wide_field(300000);
    const auto footer_only = codec->unpack(parquet(0, footer, page()), {});
    ASSERT_TRUE(footer_only) << footer_only.error().message;
    const auto header_only = codec->unpack(parquet(0, {}, page(header)), {});
    ASSERT_TRUE(header_only) << header_only.error().message;
    const auto combined = codec->unpack(parquet(0, footer, page(header)), {});
    ASSERT_FALSE(combined);
    EXPECT_NE(combined.error().message.find("structural limit"), std::string::npos);
}

TEST(StructuralLimits, NotebookRecognitionAndUnpackUseTheJsonLimit) {
    const auto source = std::string(structural_budget::maximum_depth, '[') + "0" +
                        std::string(structural_budget::maximum_depth, ']');
    const auto input = std::span{reinterpret_cast<const std::byte*>(source.data()), source.size()};
    for (const auto* codec : liquid::format_registry::global().codecs()) {
        if (codec->descriptor().name != "jupyter") {
            continue;
        }
        const auto recognised = codec->recognise(input, "deep.ipynb");
        ASSERT_FALSE(recognised);
        EXPECT_NE(recognised.error().message.find("structural limit"), std::string::npos);
        const auto unpacked = codec->unpack(input, {.maximum_depth = 0});
        ASSERT_FALSE(unpacked);
        EXPECT_NE(unpacked.error().message.find("structural limit"), std::string::npos);
        return;
    }
    FAIL() << "missing notebook codec";
}

TEST(StructuralLimits, SqliteBoundsInteriorPageChainsBelowThePageCount) {
    const auto* codec = codec_named("sqlite");
    ASSERT_NE(codec, nullptr);
    for (const auto interior :
         {structural_budget::maximum_depth - 1, structural_budget::maximum_depth}) {
        const auto parsed = codec->unpack(sqlite(interior), {});
        EXPECT_EQ(parsed.has_value(), interior < structural_budget::maximum_depth);
        if (!parsed) {
            EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos);
        }
    }
    const auto long_chain = codec->unpack(sqlite(10'000), {});
    ASSERT_FALSE(long_chain);
    EXPECT_NE(long_chain.error().message.find("structural limit"), std::string::npos);
}

TEST(StructuralLimits, Hdf5BoundsGroupNestingAndGroupBTreeChains) {
    const auto* codec = codec_named("hdf5");
    ASSERT_NE(codec, nullptr);
    for (const auto nodes :
         {structural_budget::maximum_depth, structural_budget::maximum_depth + 1}) {
        SCOPED_TRACE(nodes);
        auto groups = hdf5_file{};
        const auto group_root = groups.chain(nodes, [](std::optional<std::uint64_t> next) {
            return next ? object_header({{6, hard_link(*next)}}) : object_header({});
        });
        auto symbols = hdf5_file{};
        const auto btree = symbols.chain(
            nodes, [](std::optional<std::uint64_t> next) { return tree_node(0, next, 8); });
        auto heap = bytes{std::byte{'H'}, std::byte{'E'}, std::byte{'A'}, std::byte{'P'}};
        little(heap, 0, 28); // empty heap whose data starts at address zero
        auto table = bytes{};
        little(table, btree, 8);
        little(table, symbols.add(heap), 8);
        const auto symbol_root = symbols.add(object_header({{17, table}}));
        for (const auto& file : {groups.finish(group_root), symbols.finish(symbol_root)}) {
            const auto parsed = codec->unpack(file, {});
            EXPECT_EQ(parsed.has_value(), nodes == structural_budget::maximum_depth);
            if (!parsed) {
                EXPECT_NE(parsed.error().message.find("structural limit"), std::string::npos);
            }
        }
    }
}

TEST(StructuralLimits, Hdf5BoundsChunkBTreeChains) {
    const auto* codec = codec_named("hdf5");
    ASSERT_NE(codec, nullptr);
    for (const auto nodes :
         {structural_budget::maximum_depth, structural_budget::maximum_depth + 1}) {
        auto file = hdf5_file{};
        const auto tree = file.chain(
            nodes, [](std::optional<std::uint64_t> next) { return tree_node(1, next, 16); });
        auto dataspace = bytes{std::byte{2}, std::byte{1}, std::byte{0}, std::byte{1}};
        little(dataspace, 1, 8);
        auto datatype = bytes{std::byte{0x10}, std::byte{0}, std::byte{0}, std::byte{0}};
        little(datatype, 4, 4);
        auto layout = bytes{std::byte{3}, std::byte{2}, std::byte{1}};
        little(layout, tree, 8);
        little(layout, 1, 4);
        const auto dataset = file.add(object_header({{1, dataspace}, {3, datatype}, {8, layout}}));
        const auto parsed =
            codec->unpack(file.finish(file.add(object_header({{6, hard_link(dataset)}}))), {});
        ASSERT_TRUE(parsed) << parsed.error().message;
        ASSERT_EQ(parsed->entries.size(), 1U);
        EXPECT_EQ(parsed->entries.front().opaque_reason.find("structural limit") !=
                      std::string::npos,
                  nodes > structural_budget::maximum_depth)
            << parsed->entries.front().opaque_reason;
    }
}

} // namespace
