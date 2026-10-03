// liquid #328: a whole repository is copied between two open stores, from a disk store into a
// remote-shaped store held by an injected owner. The copy keeps every digest, resumes after an
// interruption copying only what is missing, writes nothing on a rerun, and a verify mode proves
// every object and state key equal and catches a tampered destination.

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "../temporary_directory.hpp"
#include "identity/taap.hpp"
#include "liquid/hash.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "remote_backend.hpp"
#include "write_once_backend.hpp"

namespace {

using liquid::test::remote_options;
using liquid::test::remote_service;

constexpr auto identifier = std::string_view{"workspace/alpha"};

std::vector<std::byte> bytes_of(std::string_view text) {
    return {reinterpret_cast<const std::byte*>(text.data()),
            reinterpret_cast<const std::byte*>(text.data() + text.size())};
}

std::string message(const liquid::error& failure) {
    return failure.message + " (" + failure.context + ")";
}

std::filesystem::path fresh(const std::string& name) {
    const auto path = liquid::test::fixture_path(name);
    std::filesystem::remove_all(path);
    std::filesystem::create_directories(path);
    return path;
}

// Incompressible bytes, so the file spans many chunks and frames.
std::vector<std::byte> pseudo_random(std::size_t size, std::uint64_t seed) {
    auto output = std::vector<std::byte>(size);
    auto state = seed | 1U;
    for (auto& value : output) {
        state ^= state << 13U;
        state ^= state >> 7U;
        state ^= state << 17U;
        value = std::byte{static_cast<unsigned char>(state >> 32U)};
    }
    return output;
}

liquid::result<liquid::attestation> delegation(const liquid::actor& agent) {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return std::unexpected(signer.error());
    }
    auto subject = liquid::hash(liquid::algorithm::blake3, bytes_of(agent.taap_id));
    if (!subject) {
        return std::unexpected(subject.error());
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto value = liquid::attestation{.subject = *subject,
                                     .profile = "delegation/1",
                                     .at = now,
                                     .issuer = signer->identity(),
                                     .subject_kind = liquid::attestation_subject_kind::actor,
                                     .attributes = {{"scopes", std::vector<std::string>{"**"}},
                                                    {"may_publish", true},
                                                    {"may_lease", true},
                                                    {"depth_remaining", std::uint64_t{1}},
                                                    {"model", "test-agent"},
                                                    {"dispatcher", signer->identity().taap_id}}};
    auto body = liquid::objects::encode_attestation_body(value);
    if (!body) {
        return std::unexpected(body.error());
    }
    auto signed_value = liquid::runtime::block_on(signer->sign("attestation", *body));
    if (!signed_value) {
        return std::unexpected(signed_value.error());
    }
    auto signature = liquid::encode_attestation_signature(*signed_value);
    if (!signature) {
        return std::unexpected(signature.error());
    }
    value.signature = std::move(*signature);
    return value;
}

// What the source repository holds once it is built, to compare against the destination.
struct expected_repository {
    std::map<std::string, std::vector<std::byte>> files;
    std::vector<liquid::ref> refs;
    std::size_t changes{};
    std::size_t drips{};
    liquid::session_id session{};
    std::vector<std::byte> session_file;
};

// A disk store holding the repository under test and an unrelated one beside it: history, a
// rename, a multi-chunk file streamed from disk, a second ref, an uncheckpointed journal, a private
// session with its own write, and a replication cursor.
expected_repository build_source(liquid::store& objects, const std::filesystem::path& scratch) {
    auto expected = expected_repository{};
    auto options = liquid::repository_options{};
    options.store = &objects;
    options.checkpoint_interval = 1000;
    auto created = liquid::runtime::block_on(liquid::repository::create(identifier, options));
    EXPECT_TRUE(created) << message(created.error());
    if (!created) {
        return expected;
    }
    auto& repository = *created;
    const auto write = [&](const std::string& path, std::vector<std::byte> contents) {
        auto written = liquid::runtime::block_on(repository.write(path, contents));
        EXPECT_TRUE(written) << message(written.error());
        expected.files.insert_or_assign(path, std::move(contents));
    };
    write("README.md", bytes_of("# alpha\n"));
    write("src/main.cpp", bytes_of("int main() { return 0; }\n"));
    auto first = liquid::runtime::block_on(repository.cut("first"));
    EXPECT_TRUE(first) << message(first.error());

    const auto large = pseudo_random(6U * 1024U * 1024U + 4321U, 328);
    const auto large_path = scratch / "model.bin";
    {
        auto output = std::ofstream(large_path, std::ios::binary);
        output.write(reinterpret_cast<const char*>(large.data()),
                     static_cast<std::streamsize>(large.size()));
    }
    auto streamed =
        liquid::runtime::block_on(repository.write_file("models/model.bin", large_path));
    EXPECT_TRUE(streamed) << message(streamed.error());
    expected.files.insert_or_assign("models/model.bin", large);
    auto moved = liquid::runtime::block_on(repository.move("src/main.cpp", "src/app.cpp"));
    EXPECT_TRUE(moved) << message(moved.error());
    expected.files.insert_or_assign("src/app.cpp", expected.files.at("src/main.cpp"));
    expected.files.erase("src/main.cpp");
    write("README.md", bytes_of("# alpha\n\nsecond version\n"));
    auto second = liquid::runtime::block_on(repository.cut("second"));
    EXPECT_TRUE(second) << message(second.error());
    if (second) {
        auto set = liquid::runtime::block_on(repository.set_ref("release", *second));
        EXPECT_TRUE(set) << message(set.error());
    }

    auto agent = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(agent) << message(agent.error());
    if (agent) {
        auto authority = delegation(agent->identity());
        EXPECT_TRUE(authority) << message(authority.error());
        if (authority) {
            auto forked = liquid::runtime::block_on(
                repository.fork_session({.actor = agent->identity(), .delegation = *authority}));
            EXPECT_TRUE(forked) << message(forked.error());
            if (forked) {
                expected.session = forked->id();
                expected.session_file = bytes_of("private to the session\n");
                auto private_write = liquid::runtime::block_on(
                    forked->write("drafts/session.txt", expected.session_file));
                EXPECT_TRUE(private_write) << message(private_write.error());
            }
        }
    }

    auto cursor = liquid::runtime::block_on(
        repository.set_replication_cursor("peer-a", liquid::journal_cursor{"7:0"}));
    EXPECT_TRUE(cursor) << message(cursor.error());
    // Written after the last checkpoint, so it is only in the journal's document.
    write("notes/journal-only.txt", bytes_of("pending\n"));

    auto refs = liquid::runtime::block_on(repository.refs());
    EXPECT_TRUE(refs);
    if (refs) {
        expected.refs = *refs;
    }
    auto history = liquid::runtime::block_on(repository.log());
    EXPECT_TRUE(history);
    if (history) {
        expected.changes = history->size();
    }
    auto drips = liquid::runtime::block_on(repository.drips());
    EXPECT_TRUE(drips);
    if (drips) {
        expected.drips = drips->size();
    }

    auto other = liquid::runtime::block_on(liquid::repository::create("workspace/beta", options));
    EXPECT_TRUE(other) << message(other.error());
    if (other) {
        auto written =
            liquid::runtime::block_on(other->write("beta.txt", bytes_of("another repository\n")));
        EXPECT_TRUE(written) << message(written.error());
    }
    return expected;
}

std::string state_prefix(std::string_view name) {
    auto id = liquid::hash(liquid::algorithm::blake3, bytes_of(name));
    EXPECT_TRUE(id);
    const auto text = liquid::to_string(*id);
    return "repositories/" + text.substr(text.find(':') + 1) + "/";
}

// Under liquid::sim::write_once_environment the destination refuses in-place range rewrites, as
// Zetta's object storage does, since lake transfers into it (liquid #332).
liquid::test::remote_store write_once_if_requested(liquid::test::remote_store store) {
    if (liquid::sim::write_once_requested()) {
        store.options.backend =
            std::make_shared<liquid::sim::write_once_store_backend>(store.options.backend);
    }
    return store;
}

// Every disk store this suite opens: the default disk backend, or the write-once one over it.
liquid::result<liquid::store> open_disk(const std::filesystem::path& path) {
    auto options = liquid::store_options{};
    options.backend = liquid::sim::test_store_backend(path);
    return liquid::runtime::block_on(liquid::store::open(path, options));
}

// The destination seals a pack every MiB, so a copy spans several packs.
liquid::test::remote_store destination_options(const std::shared_ptr<remote_service>& service) {
    auto output = remote_options(service);
    output.options.pack_size = 1024U * 1024U;
    return write_once_if_requested(std::move(output));
}

bool has_key(const std::shared_ptr<remote_service>& service, const std::string& key) {
    auto lock = std::scoped_lock(service->mutex);
    return service->objects.contains(key);
}

// Opens the copied repository on the destination and compares it with the source.
void check_destination(liquid::store& objects, const expected_repository& expected) {
    auto options = liquid::repository_options{};
    options.store = &objects;
    options.checkpoint_interval = 1000;
    auto opened = liquid::runtime::block_on(liquid::repository::open(identifier, options));
    ASSERT_TRUE(opened) << message(opened.error());
    for (const auto& [path, contents] : expected.files) {
        auto read = liquid::runtime::block_on(opened->read(path));
        ASSERT_TRUE(read) << path << ": " << message(read.error());
        EXPECT_EQ(*read, contents) << path;
    }
    auto refs = liquid::runtime::block_on(opened->refs());
    ASSERT_TRUE(refs);
    ASSERT_EQ(refs->size(), expected.refs.size());
    for (std::size_t index = 0; index != refs->size(); ++index) {
        EXPECT_EQ((*refs)[index].name, expected.refs[index].name);
        EXPECT_EQ((*refs)[index].id.value.bytes, expected.refs[index].id.value.bytes);
    }
    auto history = liquid::runtime::block_on(opened->log());
    ASSERT_TRUE(history);
    EXPECT_EQ(history->size(), expected.changes);
    auto drips = liquid::runtime::block_on(opened->drips());
    ASSERT_TRUE(drips);
    EXPECT_EQ(drips->size(), expected.drips);
    auto cursor = liquid::runtime::block_on(opened->replication_cursor("peer-a"));
    ASSERT_TRUE(cursor);
    ASSERT_TRUE(cursor->has_value());
    EXPECT_EQ((*cursor)->token, "7:0");
    if (!expected.session_file.empty()) {
        auto session = liquid::runtime::block_on(opened->open_session(expected.session));
        ASSERT_TRUE(session) << message(session.error());
        auto read = liquid::runtime::block_on(session->read("drafts/session.txt"));
        ASSERT_TRUE(read) << message(read.error());
        EXPECT_EQ(*read, expected.session_file);
    }
    auto dry = liquid::runtime::block_on(opened->collect({.dry_run = true}));
    ASSERT_TRUE(dry) << message(dry.error());
}

liquid::repository_transfer_report
run_transfer(liquid::store& from,
             liquid::store& into,
             liquid::repository_transfer_mode mode = liquid::repository_transfer_mode::copy) {
    auto options = liquid::repository_transfer_options{.mode = mode, .batch_objects = 8};
    auto report =
        liquid::runtime::block_on(liquid::repository::transfer(from, into, identifier, options));
    EXPECT_TRUE(report) << message(report.error());
    return report ? *report : liquid::repository_transfer_report{};
}

std::string describe(const liquid::repository_transfer_report& report) {
    auto output = std::string{};
    for (const auto& difference : report.differences) {
        output += difference.subject + ": " + difference.reason + "\n";
    }
    return output;
}

TEST(RepositoryTransfer, InterruptedCopyResumesAndVerifies) {
    const auto root = fresh("repository-transfer-resume");
    auto source = open_disk(root / "source");
    ASSERT_TRUE(source) << message(source.error());
    const auto expected = build_source(*source, root);
    ASSERT_FALSE(HasFailure());
    const auto prefix = state_prefix(identifier);

    // An uninterrupted copy measures how many backend mutations a whole transfer makes.
    auto reference = std::make_shared<remote_service>();
    auto reference_options = destination_options(reference);
    auto whole = liquid::runtime::block_on(
        liquid::store::open(root / "reference", reference_options.options));
    ASSERT_TRUE(whole) << message(whole.error());
    const auto before = reference->accepted;
    const auto full = run_transfer(*source, *whole);
    ASSERT_TRUE(full.complete);
    const auto mutations = reference->accepted - before;
    EXPECT_GT(full.objects_written, 0U);
    EXPECT_GT(full.bytes_written, 6U * 1024U * 1024U);
    EXPECT_TRUE(full.missing.empty());
    EXPECT_GT(full.state_written, 3U);
    EXPECT_GT(full.leases_written, 0U);
    {
        auto lock = std::scoped_lock(reference->mutex);
        for (const auto& [key, bytes] : reference->objects) {
            static_cast<void>(bytes);
            EXPECT_FALSE(key.starts_with("repositories/") && !key.starts_with(prefix))
                << "only the transferred repository's state is copied: " << key;
        }
    }

    for (const auto fraction : {0.1, 0.35, 0.6, 0.9, 0.99}) {
        SCOPED_TRACE(fraction);
        auto service = std::make_shared<remote_service>();
        auto stale_options = destination_options(service);
        auto stale = liquid::runtime::block_on(
            liquid::store::open(root / "destination", stale_options.options));
        ASSERT_TRUE(stale) << message(stale.error());
        // The owner's lease lapses part way through, as if its renewal had failed: the remote
        // refuses the next write and the transfer stops there.
        const auto lapse = std::max<std::uint64_t>(
            1, static_cast<std::uint64_t>(static_cast<double>(mutations) * fraction));
        service->expire_at = service->accepted + lapse;
        auto interrupted = liquid::runtime::block_on(
            liquid::repository::transfer(*source, *stale, identifier, {.batch_objects = 8}));
        ASSERT_FALSE(interrupted);
        EXPECT_EQ(interrupted.error().code, liquid::errc::locked) << message(interrupted.error());
        EXPECT_GT(service->refused, 0U);
        // LIQUID_REPO is written last, so an interrupted copy names no repository at all.
        EXPECT_FALSE(has_key(service, prefix + "LIQUID_REPO"));
        auto closed = liquid::runtime::block_on(stale->close());
        ASSERT_TRUE(closed) << message(closed.error());

        // A successor takes the store with a higher fence and resumes.
        auto successor_options = destination_options(service);
        auto successor = liquid::runtime::block_on(
            liquid::store::open(root / "destination", successor_options.options));
        ASSERT_TRUE(successor) << message(successor.error());
        EXPECT_GT(successor_options.owner->grant().fence(), stale_options.owner->grant().fence());
        const auto resumed = run_transfer(*source, *successor);
        ASSERT_TRUE(resumed.complete);
        EXPECT_LT(resumed.bytes_written, full.bytes_written)
            << "a rerun copies only what is missing";
        {
            auto lock = std::scoped_lock(service->mutex);
            EXPECT_EQ(service->written_with.at(prefix + "LIQUID_REPO"),
                      successor_options.owner->grant().fence());
        }

        const auto verified =
            run_transfer(*source, *successor, liquid::repository_transfer_mode::verify);
        EXPECT_TRUE(verified.complete) << describe(verified);
        EXPECT_TRUE(verified.differences.empty()) << describe(verified);
        EXPECT_EQ(verified.objects, full.objects);
        EXPECT_EQ(verified.objects_written, 0U);

        // Idempotent: a third run finds everything present and writes nothing.
        const auto accepted = service->accepted;
        const auto again = run_transfer(*source, *successor);
        ASSERT_TRUE(again.complete);
        EXPECT_EQ(again.objects_written, 0U);
        EXPECT_EQ(again.bytes_written, 0U);
        EXPECT_EQ(again.state_written, 0U);
        EXPECT_EQ(again.leases_written, 0U);
        EXPECT_GT(again.objects_present, 0U);
        EXPECT_EQ(service->accepted, accepted);

        // The destination reopens on its own and holds the same repository.
        auto copied = std::move(*successor);
        auto released = liquid::runtime::block_on(copied.close());
        ASSERT_TRUE(released) << message(released.error());
        auto reader_options = destination_options(service);
        auto reader = liquid::runtime::block_on(
            liquid::store::open(root / "destination", reader_options.options));
        ASSERT_TRUE(reader) << message(reader.error());
        const auto reverified =
            run_transfer(*source, *reader, liquid::repository_transfer_mode::verify);
        EXPECT_TRUE(reverified.complete) << describe(reverified);
        check_destination(*reader, expected);
        auto done = liquid::runtime::block_on(reader->close());
        ASSERT_TRUE(done) << message(done.error());
    }
}

TEST(RepositoryTransfer, VerifyCatchesTamperedDestination) {
    const auto root = fresh("repository-transfer-tamper");
    auto source = open_disk(root / "source");
    ASSERT_TRUE(source) << message(source.error());
    static_cast<void>(build_source(*source, root));
    ASSERT_FALSE(HasFailure());
    const auto prefix = state_prefix(identifier);

    auto service = std::make_shared<remote_service>();
    auto options = write_once_if_requested(remote_options(service));
    auto destination =
        liquid::runtime::block_on(liquid::store::open(root / "destination", options.options));
    ASSERT_TRUE(destination) << message(destination.error());

    // Nothing copied yet: verify reports the missing state and objects and writes nothing.
    const auto empty =
        run_transfer(*source, *destination, liquid::repository_transfer_mode::verify);
    EXPECT_FALSE(empty.complete);
    EXPECT_FALSE(has_key(service, prefix + "LIQUID_REPO"));
    EXPECT_TRUE(std::ranges::any_of(empty.differences, [&](const auto& difference) {
        return difference.subject == prefix + "LIQUID_REPO";
    }));

    const auto copied = run_transfer(*source, *destination);
    ASSERT_TRUE(copied.complete);
    const auto clean =
        run_transfer(*source, *destination, liquid::repository_transfer_mode::verify);
    ASSERT_TRUE(clean.complete) << describe(clean);

    // One byte flipped inside a stored frame: the object that chunk belongs to no longer verifies.
    {
        auto lock = std::scoped_lock(service->mutex);
        auto& pack = service->objects.at("packs/pack-1.pack");
        ASSERT_GT(pack.size(), 1024U);
        pack[pack.size() / 2] ^= std::byte{0x5a};
    }
    const auto tampered =
        run_transfer(*source, *destination, liquid::repository_transfer_mode::verify);
    EXPECT_FALSE(tampered.complete);
    EXPECT_FALSE(tampered.differences.empty());
    EXPECT_TRUE(std::ranges::any_of(tampered.differences, [](const auto& difference) {
        return difference.reason.find("verify") != std::string::npos;
    })) << describe(tampered);

    // A state key the destination changed is reported by key.
    {
        auto lock = std::scoped_lock(service->mutex);
        service->objects.at(prefix + "replication/peer-a.cursor") = bytes_of("9:9");
    }
    const auto state =
        run_transfer(*source, *destination, liquid::repository_transfer_mode::verify);
    EXPECT_TRUE(std::ranges::any_of(state.differences, [&](const auto& difference) {
        return difference.subject == prefix + "replication/peer-a.cursor" &&
               difference.reason == "state key differs";
    })) << describe(state);
}

TEST(RepositoryTransfer, RefusesMissingRepositoryAndOneStore) {
    const auto root = fresh("repository-transfer-refusals");
    auto source = open_disk(root / "source");
    ASSERT_TRUE(source) << message(source.error());
    auto destination = open_disk(root / "destination");
    ASSERT_TRUE(destination) << message(destination.error());

    auto missing = liquid::runtime::block_on(
        liquid::repository::transfer(*source, *destination, "workspace/absent"));
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, liquid::errc::not_found);

    auto same =
        liquid::runtime::block_on(liquid::repository::transfer(*source, *source, identifier));
    ASSERT_FALSE(same);
    EXPECT_EQ(same.error().code, liquid::errc::invalid_argument);

    auto unbatched = liquid::runtime::block_on(
        liquid::repository::transfer(*source, *destination, identifier, {.batch_objects = 0}));
    ASSERT_FALSE(unbatched);
    EXPECT_EQ(unbatched.error().code, liquid::errc::invalid_argument);
}

} // namespace
