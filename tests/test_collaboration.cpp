// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <string>
#include <string_view>

#include "identity/taap.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "objects/records.hpp"
#include "temporary_directory.hpp"

namespace {

class collaboration_directory {
public:
    collaboration_directory()
        : path_(
              liquid::test::fixture_path("liquid-collaboration-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~collaboration_directory() {
        auto code = std::error_code{};
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

auto text(const std::vector<std::byte>& value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto source_range(std::uint64_t start, std::uint64_t end) -> liquid::frontend_source_range {
    return {.start = {.byte = start, .line = 1, .column = start + 1},
            .end = {.byte = end, .line = 1, .column = end + 1}};
}

auto delegation(const liquid::actor& agent,
                std::vector<std::string> scopes,
                bool may_publish = true,
                bool may_lease = true) -> liquid::result<liquid::attestation> {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return std::unexpected(signer.error());
    }
    auto subject = liquid::hash(liquid::algorithm::blake3, bytes(agent.taap_id));
    if (!subject) {
        return std::unexpected(subject.error());
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto value = liquid::attestation{.subject = *subject,
                                     .profile = "delegation/1",
                                     .at = now,
                                     .issuer = signer->identity(),
                                     .subject_kind = liquid::attestation_subject_kind::actor,
                                     .attributes = {{"scopes", std::move(scopes)},
                                                    {"may_publish", may_publish},
                                                    {"may_lease", may_lease},
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

auto create_repository(const std::filesystem::path& path) -> liquid::repository {
    auto created = liquid::runtime::block_on(liquid::repository::create(path));
    EXPECT_TRUE(created) << created.error().message;
    return created ? std::move(*created) : liquid::repository{};
}

auto current_actor() -> liquid::actor {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(signer) << signer.error().message;
    return signer ? signer->identity() : liquid::actor{};
}

TEST(Collaboration, speculative_writes_are_private_and_publish_is_attributed_and_scoped) {
    collaboration_directory directory;
    auto opened_store =
        liquid::runtime::block_on(liquid::store::open(directory.path() / "shared-store"));
    ASSERT_TRUE(opened_store);
    auto shared_store = std::move(*opened_store);
    auto options = liquid::repository_options{.store = &shared_store};
    auto first_person =
        liquid::runtime::block_on(liquid::repository::create("person-one", options));
    auto second_person =
        liquid::runtime::block_on(liquid::repository::create("person-two", options));
    auto created_shared =
        liquid::runtime::block_on(liquid::repository::create("agent-repository", options));
    ASSERT_TRUE(first_person);
    ASSERT_TRUE(second_person);
    ASSERT_TRUE(created_shared);
    auto shared = std::move(*created_shared);
    ASSERT_TRUE(liquid::runtime::block_on(first_person->write("notes/one.txt", bytes("one\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(second_person->write("notes/two.txt", bytes("two\n"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(shared.write("src/value.cpp", bytes("int value = 1;\n"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(shared.write("src/other.cpp", bytes("int other = 1;\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(shared.cut("fork point")));
    auto before = liquid::runtime::block_on(shared.drips());
    ASSERT_TRUE(before);
    ASSERT_FALSE(before->empty());
    EXPECT_FALSE(before->back().delegation.has_value());

    const auto agent = current_actor();
    auto authority = delegation(agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;
    auto forked =
        liquid::runtime::block_on(shared.fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto agent_session = std::move(*forked);
    ASSERT_TRUE(
        liquid::runtime::block_on(agent_session.write("src/value.cpp", bytes("int value = 2;\n"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(agent_session.write("src/value.cpp", bytes("int value = 3;\n"))));
    ASSERT_TRUE(
        liquid::runtime::block_on(agent_session.write("src/other.cpp", bytes("int other = 2;\n"))));

    auto private_read = liquid::runtime::block_on(agent_session.read("src/value.cpp"));
    auto shared_read = liquid::runtime::block_on(shared.read("src/value.cpp"));
    auto still_private = liquid::runtime::block_on(shared.drips());
    ASSERT_TRUE(private_read);
    ASSERT_TRUE(shared_read);
    ASSERT_TRUE(still_private);
    EXPECT_EQ(text(*private_read), "int value = 3;\n");
    EXPECT_EQ(text(*shared_read), "int value = 1;\n");
    EXPECT_EQ(still_private->size(), before->size());

    const auto persisted_id = agent_session.id();
    agent_session = liquid::session{};
    auto reopened = liquid::runtime::block_on(shared.open_session(persisted_id));
    ASSERT_TRUE(reopened) << reopened.error().message;
    agent_session = std::move(*reopened);
    auto reopened_read = liquid::runtime::block_on(agent_session.read("src/value.cpp"));
    ASSERT_TRUE(reopened_read) << reopened_read.error().message;
    EXPECT_EQ(text(*reopened_read), "int value = 3;\n");

    auto published = liquid::runtime::block_on(
        agent_session.publish({.message = "agent update", .atomic = true}));
    ASSERT_TRUE(published) << published.error().message;
    EXPECT_EQ(published->directories, 1U);
    ASSERT_EQ(published->paths.size(), 2U);
    for (const auto& path : published->paths) {
        EXPECT_TRUE(path.applied) << path.reason;
    }
    auto landed = liquid::runtime::block_on(shared.read("src/value.cpp"));
    auto drips = liquid::runtime::block_on(shared.drips());
    ASSERT_TRUE(landed) << landed.error().message << ": " << landed.error().context;
    ASSERT_TRUE(drips);
    EXPECT_EQ(text(*landed), "int value = 3;\n");
    ASSERT_FALSE(drips->empty());
    EXPECT_EQ(drips->size(), before->size() + 1);
    EXPECT_EQ(drips->back().by.taap_id, agent.taap_id);
    EXPECT_TRUE(drips->back().delegation.has_value());

    auto restricted = delegation(agent, {"docs/**"});
    ASSERT_TRUE(restricted);
    auto restricted_fork =
        liquid::runtime::block_on(shared.fork_session({.actor = agent, .delegation = *restricted}));
    ASSERT_TRUE(restricted_fork);
    auto restricted_session = std::move(*restricted_fork);
    ASSERT_TRUE(liquid::runtime::block_on(
        restricted_session.write("src/value.cpp", bytes("int value = 4;\n"))));
    auto refused = liquid::runtime::block_on(restricted_session.publish());
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    auto unchanged = liquid::runtime::block_on(shared.read("src/value.cpp"));
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(text(*unchanged), "int value = 3;\n");
    EXPECT_TRUE(liquid::runtime::block_on(restricted_session.discard()));
}

TEST(Collaboration, line_attribution_follows_changes_sessions_agents_and_moves) {
    collaboration_directory directory;
    auto human_session = liquid::parse_session_id("11111111111111111111111111111111");
    ASSERT_TRUE(human_session);
    auto options = liquid::repository_options{};
    options.session = *human_session;
    auto created = liquid::runtime::block_on(
        liquid::repository::create(directory.path() / "attribution", options));
    ASSERT_TRUE(created);
    auto shared = std::move(*created);
    ASSERT_TRUE(
        liquid::runtime::block_on(shared.write("src/value.cpp", bytes("first\nsecond\nthird\n"))));
    auto initial = liquid::runtime::block_on(shared.cut("initial"));
    ASSERT_TRUE(initial);

    const auto agent = current_actor();
    auto authority = delegation(agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto forked =
        liquid::runtime::block_on(shared.fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_TRUE(forked);
    auto agent_session = std::move(*forked);
    const auto agent_id = agent_session.id();
    ASSERT_TRUE(liquid::runtime::block_on(
        agent_session.write("src/value.cpp", bytes("first\nchanged\nthird\n"))));
    auto published =
        liquid::runtime::block_on(agent_session.publish({.message = "agent line", .atomic = true}));
    ASSERT_TRUE(published);
    ASSERT_TRUE(published->change);

    auto attributed =
        liquid::runtime::block_on(shared.attribution("src/value.cpp", *published->change));
    ASSERT_TRUE(attributed) << attributed.error().message;
    ASSERT_EQ(attributed->size(), 3U);
    EXPECT_EQ(attributed->at(0).text, "first");
    EXPECT_EQ(attributed->at(0).session, *human_session);
    EXPECT_EQ(liquid::to_string(attributed->at(0).change.value), liquid::to_string(initial->value));
    EXPECT_FALSE(attributed->at(0).agent);
    EXPECT_EQ(attributed->at(1).text, "changed");
    EXPECT_EQ(attributed->at(1).session, agent_id);
    EXPECT_EQ(liquid::to_string(attributed->at(1).change.value),
              liquid::to_string(published->change->value));
    EXPECT_TRUE(attributed->at(1).agent);
    EXPECT_EQ(attributed->at(2).session, *human_session);

    auto reopened =
        liquid::runtime::block_on(liquid::repository::open(directory.path() / "attribution"));
    ASSERT_TRUE(reopened);
    auto persisted =
        liquid::runtime::block_on(reopened->attribution("src/value.cpp", *published->change));
    ASSERT_TRUE(persisted) << persisted.error().message;
    ASSERT_EQ(persisted->size(), attributed->size());
    EXPECT_EQ(persisted->at(1).session, agent_id);
    EXPECT_TRUE(persisted->at(1).agent);

    ASSERT_TRUE(liquid::runtime::block_on(shared.move("src/value.cpp", "src/renamed.cpp")));
    auto moved = liquid::runtime::block_on(shared.cut("rename"));
    ASSERT_TRUE(moved);
    auto after_move = liquid::runtime::block_on(shared.attribution("src/renamed.cpp", *moved));
    ASSERT_TRUE(after_move) << after_move.error().message;
    ASSERT_EQ(after_move->size(), attributed->size());
    for (auto index = std::size_t{}; index < attributed->size(); ++index) {
        EXPECT_EQ(after_move->at(index).session, attributed->at(index).session);
        EXPECT_EQ(liquid::to_string(after_move->at(index).change.value),
                  liquid::to_string(attributed->at(index).change.value));
    }
}

TEST(Collaboration, symbol_leases_refuse_intersections_allow_other_ranges_and_expire) {
    collaboration_directory directory;
    auto shared = create_repository(directory.path() / "shared");
    const auto source = std::string{"int answer = 42; int other = 7;\n"};
    ASSERT_TRUE(liquid::runtime::block_on(shared.write("src/value.cpp", bytes(source))));
    ASSERT_TRUE(liquid::runtime::block_on(shared.cut("fork point")));
    auto subject = liquid::hash(liquid::algorithm::blake3, bytes(source));
    ASSERT_TRUE(subject);
    auto stored =
        liquid::runtime::block_on(shared.store_symbols({*subject,
                                                        "c++",
                                                        liquid::symbol_origin::compiler,
                                                        {},
                                                        {{.symbol = "c++:answer",
                                                          .name = "answer",
                                                          .kind = liquid::symbol_kind::variable,
                                                          .role = liquid::symbol_role::definition,
                                                          .range = source_range(4, 10)}}}));
    ASSERT_TRUE(stored) << stored.error().message;

    const auto agent = current_actor();
    auto authority = delegation(agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto first =
        liquid::runtime::block_on(shared.fork_session({.actor = agent, .delegation = *authority}));
    auto second =
        liquid::runtime::block_on(shared.fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_TRUE(first);
    ASSERT_TRUE(second);
    auto first_agent = std::move(*first);
    auto second_agent = std::move(*second);
    const auto claim = liquid::lease_claim{
        .path = "src/value.cpp", .kind = liquid::lease_kind::symbol, .symbol = "answer"};
    ASSERT_TRUE(liquid::runtime::block_on(first_agent.lease(claim, std::chrono::seconds{1})));
    auto collision = liquid::runtime::block_on(second_agent.lease(claim, std::chrono::seconds{1}));
    ASSERT_FALSE(collision);
    EXPECT_EQ(collision.error().code, liquid::errc::locked);

    auto intersecting = liquid::write_options{};
    intersecting.range = source_range(5, 8);
    auto blocked =
        liquid::runtime::block_on(shared.write("src/value.cpp", bytes(source), intersecting));
    ASSERT_FALSE(blocked);
    EXPECT_EQ(blocked.error().code, liquid::errc::locked);

    auto elsewhere = liquid::write_options{};
    elsewhere.range = source_range(20, 25);
    EXPECT_TRUE(liquid::runtime::block_on(shared.write("src/value.cpp", bytes(source), elsewhere)));

    auto delayed = liquid::runtime::block_on([]() -> liquid::runtime::task<void> {
        auto [delay_error] = co_await liquid::runtime::delay(std::chrono::milliseconds{1100});
        if (delay_error) {
            co_return std::unexpected(
                liquid::error{liquid::errc::internal, delay_error.message(), "lease test"});
        }
        co_return liquid::result<void>{};
    }());
    ASSERT_TRUE(delayed);
    EXPECT_TRUE(liquid::runtime::block_on(second_agent.lease(claim, std::chrono::seconds{5})));
}

TEST(Collaboration, authoritative_lease_refusal_wins_before_local_state_changes) {
    collaboration_directory directory;
    auto refuse = true;
    auto options = liquid::repository_options{};
    options.authorize_lease = [&refuse](liquid::semantic_lease) -> liquid::runtime::task<void> {
        if (refuse) {
            co_return std::unexpected(
                liquid::error{liquid::errc::locked, "authoritative peer refused claim", "src/a"});
        }
        co_return liquid::result<void>{};
    };
    auto created =
        liquid::runtime::block_on(liquid::repository::create(directory.path() / "shared", options));
    ASSERT_TRUE(created);
    auto shared = std::move(*created);
    const auto claim = liquid::lease_claim{.path = "src/a", .kind = liquid::lease_kind::file};
    auto refused = liquid::runtime::block_on(shared.lease(claim, std::chrono::seconds{30}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::locked);
    auto absent = liquid::runtime::block_on(shared.leases());
    ASSERT_TRUE(absent);
    EXPECT_TRUE(absent->empty());

    refuse = false;
    ASSERT_TRUE(liquid::runtime::block_on(shared.lease(claim, std::chrono::seconds{30})));
    auto accepted = liquid::runtime::block_on(shared.leases());
    ASSERT_TRUE(accepted);
    ASSERT_EQ(accepted->size(), 1U);
    EXPECT_EQ(accepted->front().authority, liquid::lease_authority::lake);
}

TEST(Collaboration, presence_is_transient_and_crosses_repository_sync) {
    collaboration_directory directory;
    auto source = create_repository(directory.path() / "source");
    ASSERT_TRUE(liquid::runtime::block_on(source.write("src/value.cpp", bytes("int value;\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("fork point")));
    const auto agent = current_actor();
    auto authority = delegation(agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto forked =
        liquid::runtime::block_on(source.fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_TRUE(forked);
    auto agent_session = std::move(*forked);
    ASSERT_TRUE(liquid::runtime::block_on(
        agent_session.report_presence({.path = "src/value.cpp",
                                       .range = source_range(0, 9),
                                       .activity = liquid::presence_activity::editing})));
    auto local = liquid::runtime::block_on(source.presence());
    ASSERT_TRUE(local);
    ASSERT_EQ(local->size(), 1U);
    EXPECT_EQ(local->front().session, agent_session.id());

    auto peer = create_repository(directory.path() / "peer");
    auto cursor = liquid::journal_cursor{};
    auto page = std::vector<liquid::sync_item>(256);
    auto exported = liquid::runtime::block_on(source.export_drips(cursor, page));
    ASSERT_TRUE(exported) << exported.error().message;
    page.resize(exported->count);
    auto imported = liquid::runtime::block_on(peer.import_drips(page));
    ASSERT_TRUE(imported) << imported.error().message;
    auto remote = liquid::runtime::block_on(peer.presence());
    ASSERT_TRUE(remote);
    ASSERT_EQ(remote->size(), 1U);
    EXPECT_EQ(remote->front().path, "src/value.cpp");

    ASSERT_TRUE(liquid::runtime::block_on(source.report_presence(
        {.actor = agent,
         .session = agent_session.id(),
         .path = "src/value.cpp",
         .activity = liquid::presence_activity::idle,
         .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()) -
               std::chrono::seconds{31}})));
    auto expired = liquid::runtime::block_on(source.presence());
    ASSERT_TRUE(expired);
    EXPECT_TRUE(expired->empty());
}

TEST(Collaboration, epoch_compaction_bounds_history_and_old_cursor_gets_checkpoint) {
    collaboration_directory directory;
    auto acknowledged_epoch = std::uint64_t{};
    auto options = liquid::repository_options{};
    options.acknowledge_epoch =
        [&acknowledged_epoch](const liquid::epoch& value) -> liquid::runtime::task<std::uint64_t> {
        acknowledged_epoch = value.n;
        co_return std::uint64_t{1};
    };
    auto created =
        liquid::runtime::block_on(liquid::repository::create(directory.path() / "source", options));
    ASSERT_TRUE(created);
    auto source = std::move(*created);
    ASSERT_TRUE(liquid::runtime::block_on(source.write("src/value.cpp", bytes("0\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(source.cut("initial")));
    auto old_cursor = liquid::journal_cursor{};
    auto initial_page = std::vector<liquid::sync_item>(256);
    auto initial = liquid::runtime::block_on(source.export_drips(old_cursor, initial_page));
    ASSERT_TRUE(initial);
    old_cursor = initial->cursor;
    for (auto index = 0; index != 20; ++index) {
        ASSERT_TRUE(liquid::runtime::block_on(
            source.write("src/value.cpp", bytes(std::to_string(index) + "\n"))));
    }
    auto before = liquid::runtime::block_on(source.drips());
    ASSERT_TRUE(before);
    ASSERT_GT(before->size(), 10U);
    auto compacted = liquid::runtime::block_on(source.compact({.min_ack_peers = 1}));
    ASSERT_TRUE(compacted) << compacted.error().message;
    EXPECT_EQ(compacted->n, 1U);
    EXPECT_EQ(acknowledged_epoch, 1U);
    EXPECT_GT(compacted->document_bytes, 0U);
    EXPECT_LT(compacted->document_bytes, 1'200'000U);
    auto after = liquid::runtime::block_on(source.drips());
    ASSERT_TRUE(after);
    EXPECT_TRUE(after->empty());

    auto page = std::vector<liquid::sync_item>(256);
    auto resync = liquid::runtime::block_on(source.export_drips(old_cursor, page));
    ASSERT_TRUE(resync) << resync.error().message;
    EXPECT_GT(resync->count, 0U);
    EXPECT_TRUE(resync->done);
}

} // namespace
