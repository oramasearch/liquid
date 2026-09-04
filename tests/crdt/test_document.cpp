#include "crdt/document.hpp"

#include <algorithm>
#include <string>

#include <gtest/gtest.h>

namespace {

using liquid::crdt::document;
using liquid::crdt::operation;
using liquid::crdt::operation_kind;

auto repository_id() -> liquid::crdt::repo_id {
    auto id = liquid::crdt::repo_id{};
    id[0] = std::byte{0x42};
    return id;
}

auto write(std::string path, std::string ref) -> operation {
    return operation{.kind = operation_kind::write,
                     .path = std::move(path),
                     .ref = std::move(ref),
                     .mode = 0100644};
}

auto move(std::string from, std::string to) -> operation {
    return operation{.kind = operation_kind::move, .path = std::move(from), .to = std::move(to)};
}

auto create(std::string_view device) -> document {
    auto result = document::create(repository_id(), "taap:user:steve", device);
    EXPECT_TRUE(result.has_value()) << result.error().message;
    return std::move(*result);
}

void sync(document& left, document& right) {
    auto left_state = liquid::crdt::sync_state{};
    auto right_state = liquid::crdt::sync_state{};
    for (int round = 0; round < 20; ++round) {
        auto progress = false;
        auto from_left = left.generate_sync_message(left_state);
        ASSERT_TRUE(from_left.has_value()) << from_left.error().message;
        if (*from_left) {
            auto received = right.receive_sync_message(right_state, **from_left);
            ASSERT_TRUE(received.has_value()) << received.error().message;
            progress = true;
        }
        auto from_right = right.generate_sync_message(right_state);
        ASSERT_TRUE(from_right.has_value()) << from_right.error().message;
        if (*from_right) {
            auto received = left.receive_sync_message(left_state, **from_right);
            ASSERT_TRUE(received.has_value()) << received.error().message;
            progress = true;
        }
        if (!progress)
            return;
    }
    FAIL() << "sync did not quiesce";
}

TEST(CrdtDocument, actor_is_distinct_and_deterministic_per_device) {
    auto first = create("laptop");
    auto same = create("laptop");
    auto second = create("phone");
    EXPECT_EQ(first.actor(), same.actor());
    EXPECT_NE(first.actor(), second.actor());
    EXPECT_FALSE(
        std::ranges::all_of(first.actor(), [](auto byte) { return byte == std::byte{0}; }));
}

TEST(CrdtDocument, save_and_load_round_trip) {
    auto original = create("laptop");
    ASSERT_TRUE(original.transact({write("a/b.txt", "blake3:old")}));
    auto bytes = original.save();
    ASSERT_TRUE(bytes.has_value());
    auto loaded = document::load(*bytes, "taap:user:steve", "laptop");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto original_heads = original.heads();
    auto loaded_heads = loaded->heads();
    ASSERT_TRUE(original_heads && loaded_heads);
    EXPECT_EQ(*original_heads, *loaded_heads);
    EXPECT_EQ(*loaded->save(), *bytes);
    auto entry = loaded->inspect("a/b.txt");
    ASSERT_TRUE(entry && *entry);
    EXPECT_EQ((*entry)->ref, "blake3:old");
}

TEST(CrdtDocument, validation_failure_leaves_heads_and_state_untouched) {
    auto value = create("laptop");
    ASSERT_TRUE(value.transact({write("a/b.txt", "blake3:old")}));
    auto before_heads = value.heads();
    auto before_save = value.save();
    ASSERT_TRUE(before_heads && before_save);
    auto result = value.transact({move("missing", "elsewhere")});
    ASSERT_FALSE(result.has_value());
    EXPECT_EQ(result.error().code, liquid::errc::invalid_argument);
    EXPECT_EQ(*value.heads(), *before_heads);
    EXPECT_EQ(*value.save(), *before_save);
}

TEST(CrdtDocument, commit_options_reach_the_automerge_change) {
    auto value = create("laptop");
    auto applied = value.transact({write("note.txt", "blake3:note")},
                                  {.message = "record note", .time = 1'725'000'000'123});
    ASSERT_TRUE(applied) << applied.error().message;
    auto saved = value.save_compact();
    ASSERT_TRUE(saved) << saved.error().message;
    auto loaded = automerge_cpp::Document::load(*saved);
    ASSERT_TRUE(loaded) << loaded.error().message;
    const auto changes = loaded->get_changes();
    ASSERT_FALSE(changes.empty());
    EXPECT_EQ(changes.back().message, "record note");
    EXPECT_EQ(changes.back().timestamp, 1'725'000'000'123);
}

TEST(CrdtDocument, corrupt_sync_input_leaves_document_unchanged) {
    auto source = create("laptop");
    auto target = create("phone");
    ASSERT_TRUE(source.transact({write("note.txt", "blake3:note")}));
    auto source_state = liquid::crdt::sync_state{};
    auto target_state = liquid::crdt::sync_state{};
    auto outgoing = source.generate_sync_message(source_state);
    ASSERT_TRUE(outgoing && *outgoing);
    auto damaged = **outgoing;
    damaged.pop_back();
    auto before = target.save();
    ASSERT_TRUE(before);
    auto received = target.receive_sync_message(target_state, damaged);
    ASSERT_FALSE(received);
    EXPECT_EQ(received.error().code, liquid::errc::corrupt);
    EXPECT_EQ(*target.save(), *before);
}

TEST(CrdtDocument, concurrent_write_and_directory_move_converge) {
    auto left = create("laptop");
    auto base_write = left.transact({write("a/b.txt", "blake3:old")});
    ASSERT_TRUE(base_write.has_value());
    const auto original_id = base_write->front().entry_id;
    auto saved = left.save();
    ASSERT_TRUE(saved.has_value());
    auto loaded = document::load(*saved, "taap:user:steve", "phone");
    ASSERT_TRUE(loaded.has_value()) << loaded.error().message;
    auto right = std::move(*loaded);

    ASSERT_TRUE(left.transact({write("a/b.txt", "blake3:new")}));
    ASSERT_TRUE(right.transact({move("a", "c")}));
    auto left_before = left.heads();
    auto right_before = right.heads();
    ASSERT_TRUE(left_before && right_before);

    sync(left, right);

    EXPECT_EQ(*left.heads(), *right.heads());
    auto left_entry = left.inspect("c/b.txt");
    auto right_entry = right.inspect("c/b.txt");
    ASSERT_TRUE(left_entry && *left_entry);
    ASSERT_TRUE(right_entry && *right_entry);
    EXPECT_EQ((*left_entry)->id, original_id);
    EXPECT_EQ((*right_entry)->id, original_id);

    auto left_diff = left.diff(*left_before);
    auto right_diff = right.diff(*right_before);
    ASSERT_TRUE(left_diff && right_diff);
    ASSERT_EQ(left_diff->size(), 1);
    ASSERT_EQ(right_diff->size(), 1);
    EXPECT_EQ(left_diff->front().kind, operation_kind::move);
    EXPECT_EQ(left_diff->front().path, "a");
    EXPECT_EQ(left_diff->front().to, "c");
    EXPECT_EQ(right_diff->front().kind, operation_kind::write);
    EXPECT_EQ(right_diff->front().path, "a/b.txt");
}

} // namespace
