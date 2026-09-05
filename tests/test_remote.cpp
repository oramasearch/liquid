#include <gtest/gtest.h>

#include <cstdlib>
#include <filesystem>
#include <span>
#include <string_view>
#include <type_traits>
#include <vector>

#include "liquid/build.hpp"
#include "liquid/remote.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "objects/records.hpp"
#include "temporary_directory.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto raw = std::as_bytes(std::span{value});
    return {raw.begin(), raw.end()};
}

auto same_digest(const liquid::digest& left, const liquid::digest& right) -> bool {
    return left.alg == right.alg && left.bytes == right.bytes;
}

} // namespace

TEST(Remote, RemoteIsAnOpaqueMoveOnlyHandle) {
    static_assert(sizeof(liquid::remote) == sizeof(void*));
    static_assert(std::is_move_constructible_v<liquid::remote>);
    static_assert(!std::is_copy_constructible_v<liquid::remote>);
    liquid::remote value;
    SUCCEED();
}

TEST(RemoteInterop, CasAndActionCacheRoundTripWithConfiguredPeer) {
    const auto* endpoint = std::getenv("LIQUID_REAPI_ENDPOINT");
    if (!endpoint) {
        GTEST_SKIP() << "LIQUID_REAPI_ENDPOINT is not configured";
    }
    auto connected = liquid::runtime::block_on(
        liquid::remote::connect(liquid::remote_config{.endpoint = endpoint}));
    ASSERT_TRUE(connected) << connected.error().message;
    auto backend = std::move(*connected);

    const auto contents = bytes("liquid REAPI interoperability\n");
    auto native_id = liquid::hash(liquid::algorithm::blake3, contents);
    ASSERT_TRUE(native_id) << native_id.error().message;
    auto prepared = liquid::runtime::block_on(
        backend.blobs().prepare(backend.executor().instance(), {{*native_id, contents}}));
    ASSERT_TRUE(prepared) << prepared.error().message;
    auto missing = liquid::runtime::block_on(
        backend.blobs().find_missing(backend.executor().instance(), {*native_id}));
    ASSERT_TRUE(missing) << missing.error().message;
    if (!missing->empty()) {
        auto written = liquid::runtime::block_on(
            backend.blobs().write(backend.executor().instance(), *native_id, contents));
        ASSERT_TRUE(written) << written.error().message;
    }
    auto read = std::vector<std::byte>(contents.size());
    auto read_count = liquid::runtime::block_on(
        backend.blobs().read_range(backend.executor().instance(), *native_id, 0, read));
    ASSERT_TRUE(read_count) << read_count.error().message;
    EXPECT_EQ(*read_count, contents.size());
    EXPECT_EQ(read, contents);

    const auto root =
        liquid::test::fixture_path("liquid-reapi-peer-" + std::to_string(std::rand()));
    auto code = std::error_code{};
    std::filesystem::remove_all(root, code);
    auto created = liquid::runtime::block_on(liquid::repository::create(root));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("input.txt", contents)));
    auto tree = liquid::runtime::block_on(repository.root());
    ASSERT_TRUE(tree) << tree.error().message;
    ASSERT_EQ(tree->entries.size(), 1U);
    const auto blob = tree->entries.front().ref;
    auto tree_bytes = liquid::objects::encode(*tree);
    ASSERT_TRUE(tree_bytes) << tree_bytes.error().message;
    auto tree_id = liquid::objects::digest_of(liquid::algorithm::blake3, *tree_bytes);
    ASSERT_TRUE(tree_id) << tree_id.error().message;
    const auto sandbox = *native_id;
    auto input = liquid::action_input{.command = {"/bin/cp", "input.txt", "output.txt"},
                                      .outputs = {"output.txt"},
                                      .inputs = *tree_id,
                                      .toolchain = *tree_id,
                                      .platform = {.os = "linux", .arch = "x86_64"},
                                      .remote_eligible = true};
    auto action_bytes = liquid::objects::encode(liquid::objects::action_record{input, sandbox});
    ASSERT_TRUE(action_bytes) << action_bytes.error().message;
    auto action_id = liquid::objects::digest_of(liquid::algorithm::blake3, *action_bytes);
    ASSERT_TRUE(action_id) << action_id.error().message;
    auto value =
        liquid::action_result{.action = *action_id,
                              .outputs = {{.path = "output.txt", .mode = 0644, .blob = blob}},
                              .stdout_output = blob,
                              .stderr_output = blob,
                              .manifest = {{.path = "input.txt", .content = blob}},
                              .worker = "interop-test"};
    auto updated =
        liquid::runtime::block_on(backend.update_action_result(repository, input, sandbox, value));
    ASSERT_TRUE(updated) << updated.error().message;
    auto cached = liquid::runtime::block_on(backend.get_action_result(repository, input, sandbox));
    ASSERT_TRUE(cached) << cached.error().message;
    ASSERT_TRUE(*cached);
    ASSERT_EQ((*cached)->outputs.size(), 1U);
    EXPECT_EQ((*cached)->outputs.front().path, "output.txt");
    auto restored = liquid::runtime::block_on(repository.blob((*cached)->outputs.front().blob));
    ASSERT_TRUE(restored) << restored.error().message;
    EXPECT_EQ(*restored, contents);
    std::filesystem::remove_all(root, code);
}
