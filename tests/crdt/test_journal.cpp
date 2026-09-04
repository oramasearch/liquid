#include "crdt/journal.hpp"

#include <filesystem>
#include <string>

#include <gtest/gtest.h>

namespace {

TEST(CrdtJournal, replay_reproduces_document_heads) {
    auto repository = liquid::crdt::repo_id{};
    repository[0] = std::byte{0x18};
    auto original = liquid::crdt::document::create(repository, "taap:user:steve", "laptop");
    ASSERT_TRUE(original.has_value());

    auto directory = std::filesystem::temp_directory_path() / "liquid-crdt-journal-test";
    std::filesystem::create_directories(directory);
    auto path = directory / "drips.cbor";
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".cut");
    auto journal = liquid::crdt::journal{path};
    ASSERT_TRUE(journal.cut(*original, 1));

    auto base = original->heads();
    ASSERT_TRUE(base.has_value());
    auto operation = liquid::crdt::operation{
        .kind = liquid::crdt::operation_kind::write,
        .path = "notes.txt",
        .ref = "blake3:journal",
        .mode = 0100644,
    };
    auto applied = original->transact({operation});
    ASSERT_TRUE(applied.has_value());
    auto record = liquid::crdt::drip_record{
        .id = applied->front().id,
        .base = *base,
        .ops = *applied,
        .by = original->actor(),
        .at = 1'788'400'000,
    };
    ASSERT_TRUE(journal.append(record));

    auto replayed = journal.replay("taap:user:steve", "laptop");
    ASSERT_TRUE(replayed.has_value()) << replayed.error().message;
    EXPECT_EQ(*replayed->heads(), *original->heads());
    auto entry = replayed->inspect("notes.txt");
    ASSERT_TRUE(entry && *entry);
    EXPECT_EQ((*entry)->ref, "blake3:journal");

    std::filesystem::remove_all(directory);
}

} // namespace
