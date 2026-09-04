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

TEST(CrdtJournal, reads_records_from_a_plain_cursor) {
    auto repository = liquid::crdt::repo_id{};
    repository[0] = std::byte{0x19};
    auto document = liquid::crdt::document::create(repository, "taap:user:steve", "laptop");
    ASSERT_TRUE(document.has_value());
    auto directory = std::filesystem::temp_directory_path() / "liquid-crdt-journal-cursor-test";
    std::filesystem::create_directories(directory);
    auto path = directory / "drips.cbor";
    std::filesystem::remove(path);
    std::filesystem::remove(path.string() + ".cut");
    auto journal = liquid::crdt::journal{path};
    ASSERT_TRUE(journal.cut(*document, 1));

    for (const auto name : {"one.txt", "two.txt"}) {
        auto base = document->heads();
        ASSERT_TRUE(base);
        auto applied = document->transact({{.kind = liquid::crdt::operation_kind::write,
                                            .path = name,
                                            .ref = "blake3:cursor",
                                            .mode = 0100644}});
        ASSERT_TRUE(applied);
        ASSERT_TRUE(journal.append({.id = applied->front().id,
                                    .base = *base,
                                    .ops = *applied,
                                    .by = document->actor(),
                                    .at = 1'788'400'000}));
    }

    auto first = journal.read({}, 1);
    ASSERT_TRUE(first);
    ASSERT_EQ(first->first.size(), 1);
    auto second = journal.read(first->second, 1);
    ASSERT_TRUE(second);
    ASSERT_EQ(second->first.size(), 1);
    EXPECT_EQ(second->first.front().ops.front().path, "two.txt");
    auto tail = journal.end();
    ASSERT_TRUE(tail);
    EXPECT_EQ(second->second, *tail);
    std::filesystem::remove_all(directory);
}

} // namespace
