// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

#include <zlib.h>

#if !defined(_WIN32)
#include <pthread.h>
#endif

#include "../temporary_directory.hpp"
#include "git/object_store.hpp"
#include "git/tree_order.hpp"
#include "liquid/git.hpp"

namespace {

class temporary_directory {
public:
    temporary_directory()
        : path_(liquid::test::fixture_path("liquid-git-test-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
        std::filesystem::create_directories(path_ / "objects", code);
    }
    ~temporary_directory() {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }
    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

auto bytes(std::string_view value) -> std::vector<std::byte> {
    return {reinterpret_cast<const std::byte*>(value.data()),
            reinterpret_cast<const std::byte*>(value.data() + value.size())};
}

auto compressed(std::span<const std::byte> value) -> std::vector<std::byte> {
    uLongf size = compressBound(value.size());
    std::vector<std::byte> output(size);
    const auto status = compress2(reinterpret_cast<Bytef*>(output.data()),
                                  &size,
                                  reinterpret_cast<const Bytef*>(value.data()),
                                  value.size(),
                                  Z_BEST_COMPRESSION);
    EXPECT_EQ(status, Z_OK);
    output.resize(size);
    return output;
}

void write_bytes(const std::filesystem::path& path, std::span<const std::byte> value) {
    std::filesystem::create_directories(path.parent_path());
    std::ofstream output{path, std::ios::binary};
    output.write(reinterpret_cast<const char*>(value.data()), value.size());
    ASSERT_TRUE(output);
}

void append32(std::vector<std::byte>& output, std::uint32_t value) {
    for (unsigned shift : {24U, 16U, 8U, 0U})
        output.push_back(static_cast<std::byte>((value >> shift) & 0xffU));
}

void append_size(std::vector<std::byte>& output, std::uint64_t value) {
    do {
        auto byte = static_cast<unsigned char>(value & 0x7fU);
        value >>= 7U;
        if (value != 0)
            byte |= 0x80U;
        output.push_back(static_cast<std::byte>(byte));
    } while (value != 0);
}

auto blob_id(std::string_view payload) -> liquid::git_detail::sha1 {
    auto raw = bytes("blob " + std::to_string(payload.size()) + '\0');
    const auto content = bytes(payload);
    raw.insert(raw.end(), content.begin(), content.end());
    return liquid::git_detail::sha1_hash(raw);
}

// Write index v2 and pack entries directly so Git cannot reject malformed fixtures for us.
struct pack_fixture {
    std::vector<std::byte> pack = bytes("PACK");
    std::vector<liquid::git_detail::pack_index_entry> entries;

    pack_fixture() {
        append32(pack, 2);
        append32(pack, 0);
    }

    auto add(unsigned type,
             std::span<const std::byte> payload,
             const liquid::git_detail::sha1& id,
             std::span<const std::byte> base = {},
             std::optional<std::uint64_t> advertised_size = {}) -> std::uint64_t {
        const auto position = pack.size();
        entries.push_back({position, id});
        auto size = advertised_size.value_or(payload.size());
        auto byte = static_cast<unsigned char>((type << 4U) | (size & 0x0fU));
        size >>= 4U;
        if (size != 0)
            byte |= 0x80U;
        pack.push_back(static_cast<std::byte>(byte));
        if (size != 0)
            append_size(pack, size);
        pack.insert(pack.end(), base.begin(), base.end());
        const auto zipped = compressed(payload);
        pack.insert(pack.end(), zipped.begin(), zipped.end());
        return position;
    }

    void write(const std::filesystem::path& directory, std::string_view name = "fixture") {
        auto output = pack;
        const auto count = static_cast<std::uint32_t>(entries.size());
        for (unsigned i = 0; i < 4; ++i)
            output[8 + i] = static_cast<std::byte>((count >> ((3U - i) * 8U)) & 0xffU);
        // The reader reads a 64-byte entry header and leaves checksum verification to SHA-1.
        output.resize(output.size() + 64);
        write_bytes(directory / "objects" / "pack" / (std::string{name} + ".pack"), output);
        std::ranges::sort(entries, {}, &liquid::git_detail::pack_index_entry::id);
        std::vector<std::byte> index;
        append32(index, 0xff744f63U);
        append32(index, 2);
        for (unsigned first = 0; first < 256; ++first)
            append32(index,
                     static_cast<std::uint32_t>(
                         std::ranges::count_if(entries, [first](const auto& entry) {
                             return std::to_integer<unsigned>(entry.id[0]) <= first;
                         })));
        for (const auto& entry : entries)
            index.insert(index.end(), entry.id.begin(), entry.id.end());
        for (std::size_t i = 0; i < entries.size(); ++i)
            append32(index, 0);
        for (const auto& entry : entries)
            append32(index, static_cast<std::uint32_t>(entry.offset));
        write_bytes(directory / "objects" / "pack" / (std::string{name} + ".idx"), index);
    }
};

auto offset_distance(std::uint64_t distance) -> std::vector<std::byte> {
    std::vector<std::byte> output{static_cast<std::byte>(distance & 0x7fU)};
    while ((distance >>= 7U) != 0) {
        --distance;
        output.push_back(static_cast<std::byte>(0x80U | (distance & 0x7fU)));
    }
    std::ranges::reverse(output);
    return output;
}

auto add_chain(pack_fixture& fixture, unsigned length, bool offsets)
    -> std::vector<liquid::git_detail::sha1> {
    std::string payload = "a";
    std::vector<liquid::git_detail::sha1> ids{blob_id(payload)};
    auto position = fixture.add(3, bytes(payload), ids.back());
    for (unsigned link = 0; link < length; ++link) {
        std::vector<std::byte> delta;
        append_size(delta, payload.size());
        append_size(delta, payload.size() + 1);
        delta.push_back(std::byte{0xb0});
        delta.push_back(static_cast<std::byte>(payload.size() & 0xffU));
        delta.push_back(static_cast<std::byte>((payload.size() >> 8U) & 0xffU));
        delta.push_back(std::byte{1});
        delta.push_back(std::byte{'a'});
        payload += 'a';
        const auto id = blob_id(payload);
        const auto distance = offset_distance(fixture.pack.size() - position);
        position =
            offsets ? fixture.add(6, delta, id, distance) : fixture.add(7, delta, id, ids.back());
        ids.push_back(id);
    }
    return ids;
}

TEST(GitObjectStore, bounds_declared_and_actual_inflation_for_loose_and_packed_objects) {
    const auto payload = std::string(1024, 'a');
    const auto id = blob_id(payload);
    for (bool packed : {false, true}) {
        SCOPED_TRACE(packed);
        temporary_directory temporary;
        if (packed) {
            pack_fixture fixture;
            fixture.add(3, bytes(payload), id);
            fixture.write(temporary.path());
        } else {
            ASSERT_TRUE(liquid::git_detail::write_loose_object(
                temporary.path(), liquid::git_detail::object_kind::blob, bytes(payload)));
        }
        liquid::git_detail::object_store exact{temporary.path(), {.object_bytes = 1024}};
        auto value = exact.read(id);
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_EQ(value->payload, bytes(payload));
        liquid::git_detail::object_store small{temporary.path(), {.object_bytes = 1023}};
        value = small.read(id);
        ASSERT_FALSE(value);
        EXPECT_EQ(value.error().message, "Git object size limit exceeded");

        temporary_directory bomb;
        if (packed) {
            pack_fixture fixture;
            fixture.add(3, bytes(payload), id, {}, 1);
            fixture.write(bomb.path());
        } else {
            auto raw = bytes(std::string{"blob 1\0", 7} + payload);
            const auto name = liquid::git_detail::sha1_hex(id);
            write_bytes(bomb.path() / "objects" / name.substr(0, 2) / name.substr(2),
                        compressed(raw));
        }
        liquid::git_detail::object_store limited{bomb.path(), {.object_bytes = 1024}};
        value = limited.read(id);
        ASSERT_FALSE(value);
        EXPECT_EQ(value.error().message, "Git object inflation limit exceeded");
    }
}

TEST(GitObjectStore, shares_depth_limits_across_offset_and_reference_delta_chains) {
    for (bool offsets : {false, true}) {
        SCOPED_TRACE(offsets);
        temporary_directory temporary;
        pack_fixture fixture;
        const auto ids = add_chain(fixture, 3, offsets);
        fixture.write(temporary.path());
        liquid::git_detail::object_store accepted{temporary.path(), {.delta_depth = 2}};
        auto value = accepted.read(ids[2]);
        ASSERT_TRUE(value) << value.error().message;
        EXPECT_EQ(value->payload, bytes("aaa"));
        liquid::git_detail::object_store rejected{temporary.path(), {.delta_depth = 2}};
        value = rejected.read(ids[3]);
        ASSERT_FALSE(value);
        EXPECT_EQ(value.error().message, "Git delta depth limit exceeded");
    }
}

TEST(GitObjectStore, supports_the_default_delta_depth_and_refuses_the_next_link) {
    temporary_directory temporary;
    pack_fixture fixture;
    const auto ids = add_chain(fixture, 1025, false);
    fixture.write(temporary.path());
    liquid::git_detail::object_store accepted{temporary.path()};
    const auto value = accepted.read(ids[1024]);
    ASSERT_TRUE(value) << value.error().message;
    EXPECT_EQ(value->payload.size(), 1025);
    liquid::git_detail::object_store rejected{temporary.path()};
    const auto excessive = rejected.read(ids[1025]);
    ASSERT_FALSE(excessive);
    EXPECT_EQ(excessive.error().message, "Git delta depth limit exceeded");
}

#if !defined(_WIN32)
// Import resumes on runtime pool threads, whose default stack is 512 KiB on macOS.
TEST(GitObjectStore, resolves_the_default_delta_depth_on_a_pool_sized_stack) {
    for (bool offsets : {false, true}) {
        SCOPED_TRACE(offsets);
        temporary_directory temporary;
        pack_fixture fixture;
        const auto ids = add_chain(fixture, 1024, offsets);
        fixture.write(temporary.path());
        struct read_request {
            std::filesystem::path directory;
            liquid::git_detail::sha1 id;
            bool resolved{};
        } request{temporary.path(), ids.back()};
        // Isolate the old recursive reader's stack overflow so it cannot crash the test runner.
        EXPECT_EXIT(
            {
                pthread_attr_t attributes;
                pthread_attr_init(&attributes);
                pthread_attr_setstacksize(&attributes, 512 * 1024);
                pthread_t thread;
                const auto started = pthread_create(
                    &thread,
                    &attributes,
                    [](void* argument) -> void* {
                        auto& work = *static_cast<read_request*>(argument);
                        liquid::git_detail::object_store store{work.directory};
                        const auto value = store.read(work.id);
                        work.resolved = value && value->payload.size() == 1025;
                        return nullptr;
                    },
                    &request);
                std::_Exit(
                    started == 0 && pthread_join(thread, nullptr) == 0 && request.resolved ? 0 : 1);
            },
            ::testing::ExitedWithCode(0),
            "");
    }
}
#endif

TEST(GitObjectStore, charges_delta_programs_bases_and_results_to_one_read_budget) {
    for (bool offsets : {false, true}) {
        SCOPED_TRACE(offsets);
        temporary_directory temporary;
        pack_fixture fixture;
        const auto ids = add_chain(fixture, 2, offsets);
        fixture.write(temporary.path());
        // Two 7-byte delta programs, the 1-byte base, and 2+3 reconstructed bytes = 20.
        // The final object is 3 bytes: 2*3 + 14 slack admits exactly 20 bytes.
        liquid::git_detail::object_store exact{temporary.path(), {.read_slack_bytes = 14}};
        auto value = exact.read(ids[2]);
        ASSERT_TRUE(value) << value.error().message;
        // Each public read starts a new budget, including when it hits the object cache.
        ASSERT_TRUE(exact.read(ids[2]));
        liquid::git_detail::object_store small{temporary.path(), {.read_slack_bytes = 13}};
        value = small.read(ids[2]);
        ASSERT_FALSE(value);
        EXPECT_EQ(value.error().message, "Git object read budget exceeded");
    }
}

TEST(GitObjectStore, shares_the_read_budget_with_loose_reference_bases) {
    temporary_directory temporary;
    const auto base = liquid::git_detail::write_loose_object(
        temporary.path(), liquid::git_detail::object_kind::blob, bytes("a"));
    ASSERT_TRUE(base);
    const auto id = blob_id("b");
    pack_fixture fixture;
    const std::vector<std::byte> delta{std::byte{1}, std::byte{1}, std::byte{1}, std::byte{'b'}};
    fixture.add(7, delta, id, *base);
    fixture.write(temporary.path());
    // 4-byte program + 8-byte loose object + 1-byte result = 13 bytes.
    liquid::git_detail::object_store exact{temporary.path(), {.read_slack_bytes = 11}};
    const auto accepted = exact.read(id);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_EQ(accepted->payload, bytes("b"));
    liquid::git_detail::object_store small{temporary.path(), {.read_slack_bytes = 10}};
    const auto rejected = small.read(id);
    ASSERT_FALSE(rejected);
    EXPECT_EQ(rejected.error().message, "Git object read budget exceeded");
}

TEST(GitObjectStore, shares_budgets_across_mixed_delta_kinds_and_packs) {
    temporary_directory temporary;
    pack_fixture bases;
    const auto ids = add_chain(bases, 1, true);
    bases.write(temporary.path(), "bases");
    pack_fixture root;
    const auto id = blob_id("b");
    const std::vector<std::byte> delta{std::byte{2}, std::byte{1}, std::byte{1}, std::byte{'b'}};
    root.add(7, delta, id, ids.back());
    root.write(temporary.path(), "root");
    // REF program 4 + OFS program 7 + base 1 + results 2+1 = 15 bytes.
    liquid::git_detail::object_store exact{temporary.path(),
                                           {.read_slack_bytes = 13, .delta_depth = 2}};
    const auto accepted = exact.read(id);
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_EQ(accepted->payload, bytes("b"));
    liquid::git_detail::object_store small{temporary.path(), {.read_slack_bytes = 12}};
    const auto bytes_limited = small.read(id);
    ASSERT_FALSE(bytes_limited);
    EXPECT_EQ(bytes_limited.error().message, "Git object read budget exceeded");
    liquid::git_detail::object_store shallow{temporary.path(), {.delta_depth = 1}};
    const auto depth_limited = shallow.read(id);
    ASSERT_FALSE(depth_limited);
    EXPECT_EQ(depth_limited.error().message, "Git delta depth limit exceeded");
}

TEST(GitObjectStore, bounds_compressed_work_even_without_decoded_progress) {
    temporary_directory temporary;
    pack_fixture fixture;
    const auto id = blob_id("");
    fixture.add(3, {}, id);
    fixture.pack.resize(13);
    fixture.pack.push_back(std::byte{0x78});
    fixture.pack.push_back(std::byte{0x01});
    // Valid empty stored deflate blocks can consume input without producing bytes.
    for (unsigned i = 0; i < 30000; ++i)
        for (unsigned byte : {0U, 0U, 0U, 255U, 255U})
            fixture.pack.push_back(static_cast<std::byte>(byte));
    for (unsigned byte : {1U, 0U, 0U, 255U, 255U, 0U, 0U, 0U, 1U})
        fixture.pack.push_back(static_cast<std::byte>(byte));
    fixture.write(temporary.path());
    liquid::git_detail::object_store store{temporary.path()};
    const auto value = store.read(id);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().message, "Git object inflation limit exceeded");

    temporary_directory loose;
    const auto name = liquid::git_detail::sha1_hex(id);
    write_bytes(loose.path() / "objects" / name.substr(0, 2) / name.substr(2),
                std::span{fixture.pack}.subspan(13));
    liquid::git_detail::object_store loose_store{loose.path()};
    const auto headerless = loose_store.read(id);
    ASSERT_FALSE(headerless);
    EXPECT_EQ(headerless.error().message, "Git object inflation limit exceeded");
}

TEST(GitObjectStore, bounds_delta_target_lengths_and_every_copy_and_insert) {
    for (unsigned instruction : {1U, 0x90U}) {
        SCOPED_TRACE(instruction);
        temporary_directory temporary;
        pack_fixture fixture;
        const auto base = blob_id("aaaa");
        fixture.add(3, bytes("aaaa"), base);
        std::vector<std::byte> delta;
        append_size(delta, 4);
        append_size(delta, 1);
        delta.push_back(static_cast<std::byte>(instruction));
        if (instruction == 1) {
            delta.push_back(std::byte{'a'});
            delta.push_back(std::byte{1});
            delta.push_back(std::byte{'a'});
        } else {
            delta.push_back(std::byte{4});
        }
        const auto id = blob_id("invalid target");
        fixture.add(7, delta, id, base);
        fixture.write(temporary.path());
        liquid::git_detail::object_store store{temporary.path()};
        const auto value = store.read(id);
        ASSERT_FALSE(value);
        EXPECT_EQ(value.error().code, liquid::errc::corrupt);
        EXPECT_NE(value.error().message.find(instruction == 1 ? "insertion" : "copy"),
                  std::string::npos);
    }

    temporary_directory temporary;
    pack_fixture fixture;
    const auto base = blob_id("a");
    fixture.add(3, bytes("a"), base);
    std::vector<std::byte> delta;
    append_size(delta, 1);
    append_size(delta, 33);
    const auto id = blob_id("oversized target");
    fixture.add(7, delta, id, base);
    fixture.write(temporary.path());
    liquid::git_detail::object_store store{temporary.path(), {.object_bytes = 32}};
    const auto value = store.read(id);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().message, "Git delta result limit exceeded");
}

TEST(GitObjectStore, refuses_nonprogressing_offset_delta) {
    temporary_directory temporary;
    pack_fixture fixture;
    const auto id = blob_id("self");
    fixture.add(6, bytes("unused"), id, bytes(std::string(1, '\0')));
    fixture.write(temporary.path());
    // Isolate the old reader's unbounded recursion so it cannot crash the test runner.
    EXPECT_EXIT(
        {
            liquid::git_detail::object_store store{temporary.path()};
            const auto value = store.read(id);
            std::_Exit(!value && value.error().message == "invalid OFS_DELTA base" ? 0 : 1);
        },
        ::testing::ExitedWithCode(0),
        "");
}

TEST(GitObjectStore, refuses_cyclic_reference_deltas_across_packs) {
    temporary_directory temporary;
    const auto first = blob_id("first");
    const auto second = blob_id("second");
    pack_fixture left;
    left.add(7, bytes("unused"), first, second);
    left.write(temporary.path(), "left");
    pack_fixture right;
    right.add(7, bytes("unused"), second, first);
    right.write(temporary.path(), "right");
    EXPECT_EXIT(
        {
            liquid::git_detail::object_store store{temporary.path()};
            const auto value = store.read(first);
            std::_Exit(!value && value.error().message == "cyclic Git delta base" ? 0 : 1);
        },
        ::testing::ExitedWithCode(0),
        "");
}

TEST(GitObjectStore, refuses_delta_result_lengths_before_allocation) {
    temporary_directory temporary;
    pack_fixture fixture;
    const auto base = blob_id("a");
    fixture.add(3, bytes("a"), base);
    std::vector<std::byte> delta;
    append_size(delta, 1);
    append_size(delta, std::uint64_t{1} << 63U);
    const auto id = blob_id("huge delta");
    fixture.add(7, delta, id, base);
    fixture.write(temporary.path());
    EXPECT_EXIT(
        {
            liquid::git_detail::object_store store{temporary.path()};
            const auto value = store.read(id);
            std::_Exit(!value && value.error().message == "Git delta result limit exceeded" ? 0
                                                                                            : 1);
        },
        ::testing::ExitedWithCode(0),
        "");
}

TEST(GitObjectStore, refuses_wrapping_pack_and_delta_sizes) {
    temporary_directory temporary;
    const auto id = blob_id("a");
    pack_fixture packed;
    packed.add(3, bytes("a"), id);
    packed.pack[12] = std::byte{0xb1};
    auto extension = std::vector<std::byte>(8, std::byte{0x80});
    extension.push_back(std::byte{0x10});
    packed.pack.insert(packed.pack.begin() + 13, extension.begin(), extension.end());
    packed.write(temporary.path());
    liquid::git_detail::object_store store{temporary.path()};
    auto value = store.read(id);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().code, liquid::errc::corrupt);

    temporary_directory delta_directory;
    pack_fixture deltas;
    const auto base = blob_id("");
    deltas.add(3, {}, base);
    // Source size 2^64 wraps to zero in a reader that only checks shift counts.
    auto delta = std::vector<std::byte>(9, std::byte{0x80});
    delta.push_back(std::byte{0x02});
    append_size(delta, 1);
    delta.push_back(std::byte{1});
    delta.push_back(std::byte{'a'});
    deltas.add(7, delta, id, base);
    deltas.write(delta_directory.path());
    liquid::git_detail::object_store delta_store{delta_directory.path()};
    value = delta_store.read(id);
    ASSERT_FALSE(value);
    EXPECT_EQ(value.error().code, liquid::errc::corrupt);
}

auto run(std::string command) -> bool {
    return std::system(command.c_str()) == 0;
}

auto capture(const std::string& command) -> std::string {
    auto output = std::string{};
    const auto pipe = popen(command.c_str(), "r");
    if (pipe == nullptr)
        return output;
    std::array<char, 256> buffer{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr)
        output += buffer.data();
    if (pclose(pipe) != 0)
        return {};
    while (!output.empty() && (output.back() == '\n' || output.back() == '\r'))
        output.pop_back();
    return output;
}

TEST(GitObjectStore, hashes_and_verifies_loose_objects) {
    EXPECT_EQ(liquid::git_detail::sha1_hex(liquid::git_detail::sha1_hash(bytes("abc"))),
              "a9993e364706816aba3e25717850c26c9cd0d89d");
    temporary_directory temporary;
    const auto id = liquid::git_detail::write_loose_object(
        temporary.path(), liquid::git_detail::object_kind::blob, bytes("contents"));
    ASSERT_TRUE(id) << id.error().message;
    liquid::git_detail::object_store store{temporary.path()};
    const auto object = store.read(*id);
    ASSERT_TRUE(object) << object.error().message;
    EXPECT_EQ(object->kind, liquid::git_detail::object_kind::blob);
    EXPECT_EQ(object->payload, bytes("contents"));
}

TEST(GitObjectStore, orders_tree_entries_as_git_does) {
    struct entry {
        std::string_view name;
        bool directory;
    };
    auto entries =
        std::vector<entry>{{"a", false}, {"a-b", false}, {"a", true}, {"a0", false}, {"ab", false}};
    std::ranges::sort(entries, [](const entry& left, const entry& right) {
        return liquid::git_detail::tree_entry_less(
            left.name, left.directory, right.name, right.directory);
    });
    EXPECT_EQ(entries[0].name, "a");
    EXPECT_FALSE(entries[0].directory);
    EXPECT_EQ(entries[1].name, "a-b");
    EXPECT_EQ(entries[2].name, "a");
    EXPECT_TRUE(entries[2].directory);
    EXPECT_EQ(entries[3].name, "a0");
    EXPECT_EQ(entries[4].name, "ab");
}

// A Git tree payload with one entry `name` pointing at `child` (liquid #386).
auto tree_payload(std::string_view mode,
                  std::string_view name,
                  const liquid::git_detail::sha1& child) -> std::vector<std::byte> {
    auto output = bytes(std::string{mode} + " " + std::string{name});
    output.push_back(std::byte{});
    output.insert(output.end(), child.begin(), child.end());
    return output;
}

TEST(GitObjectStore, read_tree_refuses_unsafe_entry_names) {
    temporary_directory temporary;
    const auto blob = liquid::git_detail::write_loose_object(
        temporary.path(), liquid::git_detail::object_kind::blob, bytes("payload"));
    ASSERT_TRUE(blob) << blob.error().message;
    liquid::git_detail::object_store store{temporary.path()};
    for (const auto* name : {".",
                             "..",
                             "../../.bashrc",
                             "/etc/x",
                             "a/b",
                             "a\\b",
                             ".git",
                             ".GIT",
                             "git~1",
                             ".git.",
                             ".g\xe2\x80\x8cit"}) {
        const auto tree =
            liquid::git_detail::write_loose_object(temporary.path(),
                                                   liquid::git_detail::object_kind::tree,
                                                   tree_payload("100644", name, *blob));
        ASSERT_TRUE(tree) << tree.error().message;
        const auto entries = store.read_tree(*tree);
        ASSERT_FALSE(entries) << "accepted " << name;
        EXPECT_EQ(entries.error().code, liquid::errc::corrupt);
    }
    const auto safe =
        liquid::git_detail::write_loose_object(temporary.path(),
                                               liquid::git_detail::object_kind::tree,
                                               tree_payload("100644", ".gitignore", *blob));
    ASSERT_TRUE(safe);
    const auto entries = store.read_tree(*safe);
    ASSERT_TRUE(entries) << entries.error().message;
    ASSERT_EQ(entries->size(), 1U);
    EXPECT_EQ(entries->front().name, ".gitignore");
}

TEST(GitInteroperability, import_refuses_a_tree_entry_that_escapes) {
    if (!run("git --version >/dev/null 2>&1"))
        GTEST_SKIP() << "git is not installed";
    temporary_directory source;
    temporary_directory destination;
    ASSERT_TRUE(run("git -C " + source.path().string() + " init -q"));
    const auto objects = source.path() / ".git";
    const auto blob = liquid::git_detail::write_loose_object(
        objects, liquid::git_detail::object_kind::blob, bytes("export PATH=/tmp\n"));
    ASSERT_TRUE(blob) << blob.error().message;
    const auto tree =
        liquid::git_detail::write_loose_object(objects,
                                               liquid::git_detail::object_kind::tree,
                                               tree_payload("100644", "../../.bashrc", *blob));
    ASSERT_TRUE(tree) << tree.error().message;
    const auto commit = capture("git -C " + source.path().string() +
                                " -c user.name=Test -c user.email=test@example.test commit-tree " +
                                liquid::git_detail::sha1_hex(*tree) + " -m crafted");
    ASSERT_FALSE(commit.empty());
    ASSERT_TRUE(run("git -C " + source.path().string() + " update-ref refs/heads/main " + commit));

    auto created = liquid::runtime::block_on(liquid::repository::create(destination.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto imported = liquid::runtime::block_on(liquid::import_git(repository, source.path()));
    ASSERT_FALSE(imported);
    EXPECT_EQ(imported.error().code, liquid::errc::corrupt);
}

TEST(GitObjectStore, refuses_a_corrupt_loose_object) {
    temporary_directory temporary;
    const auto id = liquid::git_detail::write_loose_object(
        temporary.path(), liquid::git_detail::object_kind::blob, bytes("contents"));
    ASSERT_TRUE(id) << id.error().message;
    const auto name = liquid::git_detail::sha1_hex(*id);
    const auto path = temporary.path() / "objects" / name.substr(0, 2) / name.substr(2);
    auto output = std::fstream{path, std::ios::in | std::ios::out | std::ios::binary};
    ASSERT_TRUE(output);
    output.seekp(-1, std::ios::end);
    output.put('x');
    output.close();
    liquid::git_detail::object_store store{temporary.path()};
    const auto object = store.read(*id);
    ASSERT_FALSE(object);
    EXPECT_EQ(object.error().code, liquid::errc::corrupt);
}

TEST(GitObjectStore, reads_packed_objects_and_deltas) {
    if (!run("git --version >/dev/null 2>&1"))
        GTEST_SKIP() << "git is not installed";
    temporary_directory temporary;
    ASSERT_TRUE(run("git -C " + temporary.path().string() + " init -q"));
    ASSERT_TRUE(run("git -C " + temporary.path().string() + " config user.name Test"));
    ASSERT_TRUE(
        run("git -C " + temporary.path().string() + " config user.email test@example.test"));
    for (unsigned index = 0; index < 8; ++index) {
        auto output =
            std::ofstream{temporary.path() / "file.txt", std::ios::binary | std::ios::trunc};
        output << std::string(4096, 'a') << index << '\n';
        output.close();
        ASSERT_TRUE(run("git -C " + temporary.path().string() + " add file.txt"));
        ASSERT_TRUE(run("git -C " + temporary.path().string() + " commit -qm update"));
    }
    ASSERT_TRUE(run("git -C " + temporary.path().string() + " gc --aggressive --prune=now"));
    const auto directory = liquid::git_detail::git_directory(temporary.path());
    ASSERT_TRUE(directory) << directory.error().message;
    liquid::git_detail::object_store store{*directory};
    const auto refs = store.refs();
    ASSERT_TRUE(refs) << refs.error().message;
    ASSERT_EQ(refs->size(), 1U);
    const auto commit = store.read_commit(refs->begin()->second);
    ASSERT_TRUE(commit) << commit.error().message;
    const auto tree = store.read_tree(commit->tree);
    ASSERT_TRUE(tree) << tree.error().message;
    ASSERT_EQ(tree->size(), 1U);
    const auto blob = store.read(tree->front().id);
    ASSERT_TRUE(blob) << blob.error().message;
    EXPECT_EQ(blob->kind, liquid::git_detail::object_kind::blob);
    EXPECT_EQ(blob->payload.size(), 4098U);
}

TEST(GitInteroperability, imports_exports_and_resumes_a_complete_history) {
    if (!run("git --version >/dev/null 2>&1"))
        GTEST_SKIP() << "git is not installed";
    temporary_directory source;
    temporary_directory destination;
    ASSERT_TRUE(run("git -C " + source.path().string() + " init -q"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " config user.name Test"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " config user.email test@example.test"));
    for (unsigned index = 0; index < 20; ++index) {
        auto output = std::ofstream{source.path() / "old.txt", std::ios::binary | std::ios::trunc};
        output << "value " << index << '\n';
        output.close();
        ASSERT_TRUE(run("git -C " + source.path().string() + " add old.txt"));
        ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm update"));
    }
    ASSERT_TRUE(run("git -C " + source.path().string() + " branch topic"));
    std::error_code rename_error;
    std::filesystem::rename(source.path() / "old.txt", source.path() / "new.txt", rename_error);
    ASSERT_FALSE(rename_error) << rename_error.message();
    ASSERT_TRUE(run("git -C " + source.path().string() + " add -A"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm rename"));
    for (unsigned index = 21; index < 48; ++index) {
        auto output = std::ofstream{source.path() / "new.txt", std::ios::binary | std::ios::trunc};
        output << "value " << index << '\n';
        output.close();
        ASSERT_TRUE(run("git -C " + source.path().string() + " add new.txt"));
        ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm update"));
    }
    ASSERT_TRUE(run("git -C " + source.path().string() + " checkout -q topic"));
    {
        auto output =
            std::ofstream{source.path() / "topic.txt", std::ios::binary | std::ios::trunc};
        output << "topic\n";
    }
    ASSERT_TRUE(run("git -C " + source.path().string() + " add topic.txt"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " commit -qm topic"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " checkout -q -"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " merge --no-ff -qm merge topic"));
    ASSERT_TRUE(run("git -C " + source.path().string() + " tag v1"));
    EXPECT_EQ(capture("git -C " + source.path().string() + " rev-list --all --count"), "50");

    auto created = liquid::runtime::block_on(liquid::repository::create(destination.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto imported = liquid::runtime::block_on(liquid::import_git(repository, source.path()));
    ASSERT_TRUE(imported) << imported.error().message;
    EXPECT_EQ(imported->commits, 50U);
#if !defined(_WIN32)
    EXPECT_LT(imported->peak_anonymous_bytes, 1024ULL * 1024ULL * 1024ULL);
#endif
    const auto refs = liquid::runtime::block_on(repository.refs());
    ASSERT_TRUE(refs) << refs.error().message;
    ASSERT_EQ(refs->size(), 3U);
    const auto main = std::ranges::find_if(
        *refs, [](const auto& value) { return value.name == "master" || value.name == "main"; });
    ASSERT_NE(main, refs->end());
    const auto history = liquid::runtime::block_on(repository.log({.from = main->id}));
    ASSERT_TRUE(history) << history.error().message;
    EXPECT_EQ(history->size(), 50U);
    const auto mapped = liquid::runtime::block_on(repository.metadata_keys("git.commit."));
    ASSERT_TRUE(mapped) << mapped.error().message;
    EXPECT_EQ(mapped->size(), 50U);
    const auto contents = liquid::runtime::block_on(repository.read("new.txt"));
    ASSERT_TRUE(contents) << contents.error().message;
    EXPECT_EQ((std::string{reinterpret_cast<const char*>(contents->data()), contents->size()}),
              "value 47\n");

    const auto before_name = capture("git -C " + source.path().string() + " rev-parse HEAD~29");
    const auto after_name = capture("git -C " + source.path().string() + " rev-parse HEAD~28");
    const auto before_mapping =
        liquid::runtime::block_on(repository.metadata("git.commit." + before_name));
    const auto after_mapping =
        liquid::runtime::block_on(repository.metadata("git.commit." + after_name));
    ASSERT_TRUE(before_mapping) << before_mapping.error().message;
    ASSERT_TRUE(after_mapping) << after_mapping.error().message;
    ASSERT_TRUE(*before_mapping);
    ASSERT_TRUE(*after_mapping);
    const auto before_id = liquid::parse_digest(std::string{
        reinterpret_cast<const char*>((*before_mapping)->data()), (*before_mapping)->size()});
    const auto after_id = liquid::parse_digest(std::string{
        reinterpret_cast<const char*>((*after_mapping)->data()), (*after_mapping)->size()});
    ASSERT_TRUE(before_id) << before_id.error().message;
    ASSERT_TRUE(after_id) << after_id.error().message;
    const auto before = liquid::runtime::block_on(repository.at(liquid::change_id{*before_id}));
    const auto after = liquid::runtime::block_on(repository.at(liquid::change_id{*after_id}));
    ASSERT_TRUE(before) << before.error().message;
    ASSERT_TRUE(after) << after.error().message;
    ASSERT_EQ(before->root.entries.size(), 1U);
    ASSERT_EQ(after->root.entries.size(), 1U);
    EXPECT_EQ(before->root.entries.front().name, "old.txt");
    EXPECT_EQ(after->root.entries.front().name, "new.txt");
    EXPECT_EQ(before->root.entries.front().node, after->root.entries.front().node);

    const auto resumed = liquid::runtime::block_on(liquid::import_git(repository, source.path()));
    ASSERT_TRUE(resumed) << resumed.error().message;
    EXPECT_EQ(resumed->commits, 0U);
    EXPECT_EQ(resumed->skipped_commits, 50U);

    temporary_directory exported;
    const auto report =
        liquid::runtime::block_on(liquid::export_git(repository, exported.path(), main->name));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->commits, 50U);
    ASSERT_TRUE(run("git -C " + exported.path().string() + " fsck --no-dangling"));
    EXPECT_EQ(capture("git -C " + exported.path().string() + " rev-list --all --count"), "50");
    EXPECT_EQ(capture("git -C " + exported.path().string() + " show HEAD:new.txt"), "value 47");
}

} // namespace
