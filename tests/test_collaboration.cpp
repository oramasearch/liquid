// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <functional>
#include <memory>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <tuple>
#include <vector>

#include "identity/taap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "memory_backend.hpp"
#include "objects/records.hpp"
#include "store/faults.hpp"
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

// Signs `value` with this machine's identity, which becomes its issuer.
auto sign_delegation(liquid::attestation value) -> liquid::result<liquid::attestation> {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return std::unexpected(signer.error());
    }
    value.issuer = signer->identity();
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

auto epoch_seconds(std::chrono::sys_seconds value) -> std::uint64_t {
    return static_cast<std::uint64_t>(value.time_since_epoch().count());
}

// Replaces one attribute of `value`, or adds it.
void set_attribute(liquid::attestation& value, std::string name, liquid::attestation_value item) {
    const auto found =
        std::ranges::find(value.attributes, name, &liquid::attestation_attribute::name);
    if (found == value.attributes.end()) {
        value.attributes.push_back({std::move(name), std::move(item)});
    } else {
        found->value = std::move(item);
    }
}

// The hex id `delegation/1` names in its `repository` attribute.
auto repository_text(const liquid::repository& target) -> std::string {
    auto id = target.id();
    EXPECT_TRUE(id) << id.error().message;
    constexpr auto digits = std::string_view{"0123456789abcdef"};
    auto output = std::string{};
    for (const auto octet : id ? *id : liquid::node_id{}) {
        const auto value = std::to_integer<unsigned char>(octet);
        output.push_back(digits[value >> 4]);
        output.push_back(digits[value & 0x0f]);
    }
    return output;
}

auto delegation(const liquid::repository& target,
                const liquid::actor& agent,
                std::vector<std::string> scopes,
                bool may_publish = true,
                bool may_lease = true) -> liquid::result<liquid::attestation> {
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    if (!signer) {
        return std::unexpected(signer.error());
    }
    auto subject = liquid::delegation_subject(agent);
    if (!subject) {
        return std::unexpected(subject.error());
    }
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto value = liquid::attestation{
        .subject = *subject,
        .profile = "delegation/1",
        .at = now,
        .issuer = signer->identity(),
        .subject_kind = liquid::attestation_subject_kind::actor,
        .attributes = {{"scopes", std::move(scopes)},
                       {"may_publish", may_publish},
                       {"may_lease", may_lease},
                       {"depth_remaining", std::uint64_t{1}},
                       {"model", "test-agent"},
                       {"dispatcher", signer->identity().taap_id},
                       {"not_after", epoch_seconds(now + std::chrono::hours{1})},
                       {"repository", repository_text(target)}}};
    return sign_delegation(std::move(value));
}

// A status resolver that reports every delegation as not revoked, checked now.
auto not_revoked() -> liquid::delegation_status_resolver {
    return [](const liquid::attestation&,
              const liquid::digest&) -> liquid::result<liquid::attestation_status> {
        return liquid::attestation_status{.revoked = false,
                                          .checked_at = std::chrono::floor<std::chrono::seconds>(
                                              std::chrono::system_clock::now())};
    };
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
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;
    auto forked = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
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
    auto reopened =
        liquid::runtime::block_on(shared.open_session(persisted_id, {.status = not_revoked()}));
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

    auto restricted = delegation(shared, agent, {"docs/**"});
    ASSERT_TRUE(restricted);
    auto restricted_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *restricted, .status = not_revoked()}));
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
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto forked = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
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
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto first = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    auto second = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
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
    auto authority = delegation(source, agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto forked = liquid::runtime::block_on(
        source.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
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

// liquid #343: a session's publish and discard end by removing the session's state tree. On a
// backend that keeps only what a barrier covered, a crash right after either returned must not
// bring the session back: a resurrected published session could be published a second time, and a
// discarded one would reappear. lose_unbarriered() is that crash.
TEST(Collaboration, published_and_discarded_sessions_stay_gone_after_a_crash) {
    collaboration_directory directory;
    auto backend = std::make_shared<liquid::sim::memory_store_backend>();
    auto store_options = liquid::store_options{};
    store_options.backend = backend;
    auto opened_store = liquid::runtime::block_on(
        liquid::store::open(directory.path() / "shared-store", store_options));
    ASSERT_TRUE(opened_store) << opened_store.error().message;
    auto shared_store = std::optional<liquid::store>{std::move(*opened_store)};
    auto options = liquid::repository_options{.store = &*shared_store};
    auto created =
        liquid::runtime::block_on(liquid::repository::create("agent-repository", options));
    ASSERT_TRUE(created) << created.error().message;
    auto shared = std::optional<liquid::repository>{std::move(*created)};
    ASSERT_TRUE(
        liquid::runtime::block_on(shared->write("src/value.cpp", bytes("int value = 1;\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(shared->cut("fork point")));

    const auto agent = current_actor();
    auto authority = delegation(agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;
    auto published_fork =
        liquid::runtime::block_on(shared->fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_TRUE(published_fork) << published_fork.error().message;
    auto published_session = std::move(*published_fork);
    const auto published_id = published_session.id();
    ASSERT_TRUE(liquid::runtime::block_on(
        published_session.write("src/value.cpp", bytes("int value = 2;\n"))));
    auto discarded_fork =
        liquid::runtime::block_on(shared->fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_TRUE(discarded_fork) << discarded_fork.error().message;
    auto discarded_session = std::move(*discarded_fork);
    const auto discarded_id = discarded_session.id();
    ASSERT_TRUE(liquid::runtime::block_on(
        discarded_session.write("src/value.cpp", bytes("int value = 3;\n"))));

    auto published = liquid::runtime::block_on(
        published_session.publish({.message = "agent update", .atomic = true}));
    ASSERT_TRUE(published) << published.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(discarded_session.discard()));

    // The process stops at once; the backend keeps only what a barrier covered.
    backend->set_frozen(true);
    published_session = liquid::session{};
    discarded_session = liquid::session{};
    shared.reset();
    shared_store.reset();
    backend->set_frozen(false);
    backend->lose_unbarriered();

    opened_store = liquid::runtime::block_on(
        liquid::store::open(directory.path() / "shared-store", store_options));
    ASSERT_TRUE(opened_store) << opened_store.error().message;
    shared_store.emplace(std::move(*opened_store));
    options.store = &*shared_store;
    auto reopened =
        liquid::runtime::block_on(liquid::repository::open("agent-repository", options));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto landed = liquid::runtime::block_on(reopened->read("src/value.cpp"));
    ASSERT_TRUE(landed) << landed.error().message;
    EXPECT_EQ(text(*landed), "int value = 2;\n");
    for (const auto& id : {published_id, discarded_id}) {
        auto resurrected = liquid::runtime::block_on(reopened->open_session(id));
        if (resurrected) {
            ADD_FAILURE() << "session " << liquid::to_string(id)
                          << " came back after its state was removed";
            continue;
        }
        EXPECT_EQ(resurrected.error().code, liquid::errc::not_found);
    }
}

// liquid #344: a session's publish or discard crashed at each of its fault points, on a backend
// that then keeps what `crash_durability` allows of the writes since the last barrier. `Everything`
// keeps them all (a process crash); `Subset` keeps any subset with each key's own writes in order,
// store_backend's ordering contract, for several seeds per fault point. After either, the store and
// repository reopen, the file holds the fork point's value or the session's, a session that comes
// back can be discarded, and the repository accepts a new change.
class crash_at_step final : public liquid::store_detail::fault_injector {
public:
    explicit crash_at_step(std::uint64_t target, std::function<void()> on_crash = {})
        : target_(target), on_crash_(std::move(on_crash)) {}

    void visit(liquid::store_detail::fault_event) override {
        ++steps_;
        if (steps_ != target_ || fired_) {
            return;
        }
        fired_ = true;
        if (on_crash_) {
            on_crash_();
        }
        throw std::runtime_error("simulated session crash");
    }

    [[nodiscard]] std::uint64_t steps() const noexcept {
        return steps_;
    }

private:
    std::uint64_t target_{};
    std::function<void()> on_crash_;
    std::uint64_t steps_{};
    bool fired_{};
};

class session_injector_guard {
public:
    explicit session_injector_guard(liquid::store_detail::fault_injector* injector)
        : previous_(liquid::store_detail::set_fault_injector(injector)) {}
    ~session_injector_guard() {
        liquid::store_detail::set_fault_injector(previous_);
    }
    session_injector_guard(const session_injector_guard&) = delete;
    session_injector_guard& operator=(const session_injector_guard&) = delete;

private:
    liquid::store_detail::fault_injector* previous_{};
};

enum class session_ending : std::uint8_t { publish, discard };

using session_crash_case = std::tuple<session_ending, liquid::sim::crash_durability>;

class SessionCrashDurability : public testing::TestWithParam<session_crash_case> {};

// Runs one session up to its ending under `injector`, then crashes the backend with `durability`.
// Returns a failure message, or nothing when the recovered repository is consistent.
auto run_session_crash(const std::filesystem::path& root,
                       session_ending ending,
                       liquid::store_detail::fault_injector* injector,
                       const std::shared_ptr<liquid::sim::memory_store_backend>& backend,
                       liquid::sim::crash_durability durability,
                       std::uint64_t seed) -> std::optional<std::string> {
    auto store_options = liquid::store_options{};
    store_options.backend = backend;
    auto forked_id = liquid::session_id{};
    {
        auto opened_store = liquid::runtime::block_on(liquid::store::open(root, store_options));
        if (!opened_store) {
            return "store open failed: " + opened_store.error().message;
        }
        auto shared_store = std::optional<liquid::store>{std::move(*opened_store)};
        auto options = liquid::repository_options{.store = &*shared_store};
        auto created =
            liquid::runtime::block_on(liquid::repository::create("agent-repository", options));
        if (!created) {
            return "repository create failed: " + created.error().message;
        }
        auto shared = std::optional<liquid::repository>{std::move(*created)};
        if (!liquid::runtime::block_on(shared->write("src/value.cpp", bytes("int value = 1;\n"))) ||
            !liquid::runtime::block_on(shared->cut("fork point"))) {
            return "could not cut the fork point";
        }
        const auto agent = current_actor();
        auto authority = delegation(agent, {"src/**"});
        if (!authority) {
            return "delegation failed: " + authority.error().message;
        }
        auto forked = liquid::runtime::block_on(
            shared->fork_session({.actor = agent, .delegation = *authority}));
        if (!forked) {
            return "fork failed: " + forked.error().message;
        }
        auto session = std::move(*forked);
        forked_id = session.id();
        if (!liquid::runtime::block_on(session.write("src/value.cpp", bytes("int value = 2;\n")))) {
            return "session write failed";
        }
        {
            auto guard = session_injector_guard{injector};
            if (ending == session_ending::publish) {
                static_cast<void>(liquid::runtime::block_on(
                    session.publish({.message = "agent update", .atomic = true})));
            } else {
                static_cast<void>(liquid::runtime::block_on(session.discard()));
            }
            backend->set_frozen(true);
            session = liquid::session{};
            shared.reset();
            shared_store.reset();
        }
        backend->set_frozen(false);
    }
    const auto survived = backend->crash(durability, seed);
    const auto where = std::to_string(survived) + " unbarriered writes kept";

    auto opened_store = liquid::runtime::block_on(liquid::store::open(root, store_options));
    if (!opened_store) {
        return where + ": store reopen failed: " + opened_store.error().message;
    }
    auto shared_store = std::optional<liquid::store>{std::move(*opened_store)};
    auto options = liquid::repository_options{.store = &*shared_store};
    auto reopened =
        liquid::runtime::block_on(liquid::repository::open("agent-repository", options));
    if (!reopened) {
        return where + ": repository reopen failed: " + reopened.error().message;
    }
    auto value = liquid::runtime::block_on(reopened->read("src/value.cpp"));
    if (!value) {
        return where + ": the file is unreadable: " + value.error().message;
    }
    if (text(*value) != "int value = 1;\n" &&
        !(ending == session_ending::publish && text(*value) == "int value = 2;\n")) {
        return where + ": the file holds neither the fork point's value nor the session's";
    }
    auto back = liquid::runtime::block_on(reopened->open_session(forked_id));
    if (back) {
        auto discarded = liquid::runtime::block_on(back->discard());
        if (!discarded) {
            return where +
                   ": the session came back and cannot be discarded: " + discarded.error().message;
        }
    } else if (back.error().code != liquid::errc::not_found) {
        return where + ": reopening the session failed: " + back.error().message;
    }
    if (!liquid::runtime::block_on(reopened->write("src/after.cpp", bytes("int after;\n"))) ||
        !liquid::runtime::block_on(reopened->cut("after the crash"))) {
        return where + ": the repository does not accept a new change";
    }
    return std::nullopt;
}

TEST_P(SessionCrashDurability, CrashAtEveryFaultPointOfTheEnding) {
    const auto [ending, durability] = GetParam();
    const auto seeds = durability == liquid::sim::crash_durability::everything ? std::uint64_t{1}
                                                                               : std::uint64_t{2};
    collaboration_directory directory;
    auto steps = std::uint64_t{};
    {
        auto counter = crash_at_step{0};
        const auto failed = run_session_crash(directory.path() / "probe",
                                              ending,
                                              &counter,
                                              std::make_shared<liquid::sim::memory_store_backend>(),
                                              liquid::sim::crash_durability::everything,
                                              0);
        ASSERT_FALSE(failed) << "probe: " << *failed;
        steps = counter.steps();
    }
    ASSERT_GT(steps, 0U);
    for (auto target = std::uint64_t{1}; target <= steps; ++target) {
        for (auto seed = std::uint64_t{}; seed != seeds; ++seed) {
            auto backend = std::make_shared<liquid::sim::memory_store_backend>();
            auto crash = crash_at_step{target};
            const auto failed = run_session_crash(directory.path() / "actual",
                                                  ending,
                                                  &crash,
                                                  backend,
                                                  durability,
                                                  seed * 7919U + target);
            ASSERT_FALSE(failed) << "crash at fault point " << target << " of " << steps
                                 << ", seed " << seed << ": " << *failed;
        }
    }
}

INSTANTIATE_TEST_SUITE_P(
    Endings,
    SessionCrashDurability,
    testing::Combine(testing::Values(session_ending::publish, session_ending::discard),
                     testing::Values(liquid::sim::crash_durability::everything,
                                     liquid::sim::crash_durability::subset)),
    [](const auto& parameter) -> std::string {
        return std::string{std::get<0>(parameter.param) == session_ending::publish ? "Publish"
                                                                                   : "Discard"} +
               (std::get<1>(parameter.param) == liquid::sim::crash_durability::everything
                    ? "Everything"
                    : "Subset");
    });

// The tests below prove the must-nevers of docs/security/delegation-threat-model.md that hold in
// code. The ones that do not hold are filed as liquid#514 to liquid#517 and have no test yet.

auto forked_repository(const std::filesystem::path& path) -> liquid::repository {
    auto shared = create_repository(path);
    EXPECT_TRUE(
        liquid::runtime::block_on(shared.write("src/value.cpp", bytes("int value = 1;\n"))));
    EXPECT_TRUE(liquid::runtime::block_on(shared.write("docs/guide.md", bytes("# guide\n"))));
    EXPECT_TRUE(liquid::runtime::block_on(shared.cut("fork point")));
    return shared;
}

// Must-never D1 and D2: an altered, unsigned, empty or foreign-profile delegation never starts a
// session.
TEST(Collaboration, fork_session_refuses_altered_unsigned_and_empty_delegations) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;

    auto widened = *authority;
    widened.attributes[0].value = std::vector<std::string>{"**"};
    auto altered = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = widened, .status = not_revoked()}));
    ASSERT_FALSE(altered);
    EXPECT_EQ(altered.error().code, liquid::errc::unauthorized);

    auto publishing = delegation(shared, agent, {"src/**"}, false, false);
    ASSERT_TRUE(publishing);
    publishing->attributes[1].value = true;
    auto escalated = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *publishing, .status = not_revoked()}));
    ASSERT_FALSE(escalated);
    EXPECT_EQ(escalated.error().code, liquid::errc::unauthorized);

    auto unsigned_authority = *authority;
    unsigned_authority.signature.clear();
    auto unsigned_fork = liquid::runtime::block_on(shared.fork_session(
        {.actor = agent, .delegation = unsigned_authority, .status = not_revoked()}));
    ASSERT_FALSE(unsigned_fork);
    EXPECT_EQ(unsigned_fork.error().code, liquid::errc::unauthorized);

    auto other_profile = *authority;
    other_profile.profile = "authorship/1";
    auto profile_fork = liquid::runtime::block_on(shared.fork_session(
        {.actor = agent, .delegation = other_profile, .status = not_revoked()}));
    ASSERT_FALSE(profile_fork);
    EXPECT_EQ(profile_fork.error().code, liquid::errc::invalid_argument);

    auto empty = delegation(shared, agent, {});
    ASSERT_TRUE(empty) << empty.error().message;
    auto empty_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *empty, .status = not_revoked()}));
    ASSERT_FALSE(empty_fork);
    EXPECT_EQ(empty_fork.error().code, liquid::errc::invalid_argument);
}

// Must-never D3: a delegation issued by another identity never starts a session in this
// principal's repository.
TEST(Collaboration, fork_session_refuses_a_delegation_issued_by_another_identity) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();

    const auto* previous = std::getenv("LIQUID_HOME");
    const auto restore = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto other_home = directory.path() / "other-home";
    setenv("LIQUID_HOME", other_home.c_str(), 1);
    auto foreign = delegation(shared, agent, {"**"});
    if (restore) {
        setenv("LIQUID_HOME", restore->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    ASSERT_TRUE(foreign) << foreign.error().message;
    ASSERT_NE(foreign->issuer.taap_id, agent.taap_id);

    auto refused = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *foreign, .status = not_revoked()}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
}

// Must-never S1 and S2: a session without the publish or lease capability can neither publish
// nor lease, and a refused publish leaves the shared repository unchanged.
TEST(Collaboration, session_without_capabilities_cannot_publish_or_lease) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto read_only = delegation(shared, agent, {"src/**"}, false, false);
    ASSERT_TRUE(read_only);
    auto forked = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *read_only, .status = not_revoked()}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto agent_session = std::move(*forked);
    ASSERT_TRUE(
        liquid::runtime::block_on(agent_session.write("src/value.cpp", bytes("int value = 2;\n"))));

    auto published = liquid::runtime::block_on(agent_session.publish());
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, liquid::errc::unauthorized);
    auto unchanged = liquid::runtime::block_on(shared.read("src/value.cpp"));
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(text(*unchanged), "int value = 1;\n");

    const auto claim = liquid::lease_claim{.path = "src/value.cpp"};
    auto leased = liquid::runtime::block_on(agent_session.lease(claim, std::chrono::seconds{30}));
    ASSERT_FALSE(leased);
    EXPECT_EQ(leased.error().code, liquid::errc::unauthorized);
    auto leases = liquid::runtime::block_on(shared.leases());
    ASSERT_TRUE(leases);
    EXPECT_TRUE(leases->empty());
    EXPECT_TRUE(liquid::runtime::block_on(agent_session.discard()));
}

// Must-never S3: a lease is never granted outside the delegation's scopes.
TEST(Collaboration, session_lease_outside_delegation_scopes_is_refused) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto forked = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto agent_session = std::move(*forked);

    const auto outside = liquid::lease_claim{.path = "docs/guide.md"};
    auto refused =
        liquid::runtime::block_on(agent_session.lease(outside, std::chrono::seconds{30}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    auto leases = liquid::runtime::block_on(shared.leases());
    ASSERT_TRUE(leases);
    EXPECT_TRUE(leases->empty());

    const auto inside = liquid::lease_claim{.path = "src/value.cpp"};
    EXPECT_TRUE(liquid::runtime::block_on(agent_session.lease(inside, std::chrono::seconds{30})));
    EXPECT_TRUE(liquid::runtime::block_on(agent_session.discard()));
}

// Must-never S4: a move is checked at both ends, so a session can neither carry a file out of its
// scopes nor pull one in from outside them.
TEST(Collaboration, publish_refuses_a_move_across_the_delegation_scopes) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority);

    auto outward_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(outward_fork) << outward_fork.error().message;
    auto outward = std::move(*outward_fork);
    ASSERT_TRUE(liquid::runtime::block_on(outward.move("src/value.cpp", "docs/value.cpp")));
    auto carried = liquid::runtime::block_on(outward.publish());
    ASSERT_FALSE(carried);
    EXPECT_EQ(carried.error().code, liquid::errc::unauthorized);
    EXPECT_TRUE(liquid::runtime::block_on(outward.discard()));

    auto inward_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(inward_fork) << inward_fork.error().message;
    auto inward = std::move(*inward_fork);
    ASSERT_TRUE(liquid::runtime::block_on(inward.move("docs/guide.md", "src/guide.md")));
    auto pulled = liquid::runtime::block_on(inward.publish());
    ASSERT_FALSE(pulled);
    EXPECT_EQ(pulled.error().code, liquid::errc::unauthorized);
    EXPECT_TRUE(liquid::runtime::block_on(inward.discard()));

    auto source = liquid::runtime::block_on(shared.read("src/value.cpp"));
    auto guide = liquid::runtime::block_on(shared.read("docs/guide.md"));
    ASSERT_TRUE(source);
    ASSERT_TRUE(guide);
    EXPECT_EQ(text(*source), "int value = 1;\n");
    EXPECT_EQ(text(*guide), "# guide\n");
    EXPECT_FALSE(liquid::runtime::block_on(shared.read("docs/value.cpp")));
    EXPECT_FALSE(liquid::runtime::block_on(shared.read("src/guide.md")));
}

// Must-never M2: a session that published or was discarded is finished; its handle refuses work
// and its identifier cannot be reopened to publish again.
TEST(Collaboration, finished_sessions_cannot_be_reused_or_reopened) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority);

    auto published_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(published_fork) << published_fork.error().message;
    auto published_session = std::move(*published_fork);
    const auto published_id = published_session.id();
    ASSERT_TRUE(liquid::runtime::block_on(
        published_session.write("src/value.cpp", bytes("int value = 2;\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(published_session.publish({.message = "agent"})));
    EXPECT_FALSE(liquid::runtime::block_on(
        published_session.write("src/value.cpp", bytes("int value = 3;\n"))));
    EXPECT_FALSE(liquid::runtime::block_on(published_session.publish()));
    EXPECT_FALSE(
        liquid::runtime::block_on(shared.open_session(published_id, {.status = not_revoked()})));

    auto discarded_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(discarded_fork) << discarded_fork.error().message;
    auto discarded_session = std::move(*discarded_fork);
    const auto discarded_id = discarded_session.id();
    ASSERT_TRUE(liquid::runtime::block_on(
        discarded_session.write("src/value.cpp", bytes("int value = 4;\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(discarded_session.discard()));
    EXPECT_FALSE(liquid::runtime::block_on(discarded_session.publish()));
    EXPECT_FALSE(
        liquid::runtime::block_on(shared.open_session(discarded_id, {.status = not_revoked()})));

    auto landed = liquid::runtime::block_on(shared.read("src/value.cpp"));
    ASSERT_TRUE(landed);
    EXPECT_EQ(text(*landed), "int value = 2;\n");
}

// Must-never M3: one session never releases a lease another session holds, even under the same
// agent and delegation.
TEST(Collaboration, a_session_cannot_release_another_sessions_lease) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority);
    auto first_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    auto second_fork = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(first_fork) << first_fork.error().message;
    ASSERT_TRUE(second_fork) << second_fork.error().message;
    auto first = std::move(*first_fork);
    auto second = std::move(*second_fork);

    const auto claim = liquid::lease_claim{.path = "src/value.cpp"};
    ASSERT_TRUE(liquid::runtime::block_on(first.lease(claim, std::chrono::seconds{30})));
    auto stolen = liquid::runtime::block_on(second.release(claim));
    ASSERT_FALSE(stolen);
    EXPECT_EQ(stolen.error().code, liquid::errc::unauthorized);
    auto human = liquid::runtime::block_on(shared.release(claim));
    ASSERT_FALSE(human);
    EXPECT_EQ(human.error().code, liquid::errc::unauthorized);
    auto leases = liquid::runtime::block_on(shared.leases());
    ASSERT_TRUE(leases);
    ASSERT_EQ(leases->size(), 1U);
    EXPECT_EQ(leases->front().session, first.id());

    EXPECT_TRUE(liquid::runtime::block_on(first.discard()));
    EXPECT_TRUE(liquid::runtime::block_on(second.discard()));
    auto released = liquid::runtime::block_on(shared.leases());
    ASSERT_TRUE(released);
    EXPECT_TRUE(released->empty());
}

// Must-never D4 (liquid#514): a session never starts on a delegation whose status is missing,
// unanswered, revoked or stale.
TEST(Collaboration, fork_session_refuses_missing_revoked_and_stale_status) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());

    auto without_status =
        liquid::runtime::block_on(shared.fork_session({.actor = agent, .delegation = *authority}));
    ASSERT_FALSE(without_status);
    EXPECT_EQ(without_status.error().code, liquid::errc::unauthorized);

    const auto answer = [](liquid::result<liquid::attestation_status> value) {
        return liquid::delegation_status_resolver{
            [value](const liquid::attestation&, const liquid::digest&) { return value; }};
    };
    auto revoked = liquid::runtime::block_on(shared.fork_session(
        {.actor = agent,
         .delegation = *authority,
         .status = answer(liquid::attestation_status{.revoked = true, .checked_at = now})}));
    ASSERT_FALSE(revoked);
    EXPECT_EQ(revoked.error().code, liquid::errc::unauthorized);

    auto stale = liquid::runtime::block_on(
        shared.fork_session({.actor = agent,
                             .delegation = *authority,
                             .status = answer(liquid::attestation_status{
                                 .revoked = false, .checked_at = now - std::chrono::hours{2}})}));
    ASSERT_FALSE(stale);
    EXPECT_EQ(stale.error().code, liquid::errc::unauthorized);

    auto unanswered = liquid::runtime::block_on(
        shared.fork_session({.actor = agent,
                             .delegation = *authority,
                             .status = answer(std::unexpected(liquid::error{
                                 liquid::errc::io, "status service is unreachable", "test"}))}));
    ASSERT_FALSE(unanswered);
    EXPECT_EQ(unanswered.error().code, liquid::errc::unauthorized);

    auto current = liquid::runtime::block_on(shared.fork_session(
        {.actor = agent,
         .delegation = *authority,
         .status = answer(liquid::attestation_status{.revoked = false, .checked_at = now})}));
    ASSERT_TRUE(current) << current.error().message;
    EXPECT_TRUE(liquid::runtime::block_on(current->discard()));
}

// Must-never D4 (liquid#514): an expired, future-dated, unbounded or undated delegation never
// starts a session.
TEST(Collaboration, fork_session_refuses_expired_future_and_unbounded_delegations) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    const auto fork = [&](liquid::attestation value) {
        return liquid::runtime::block_on(shared.fork_session(
            {.actor = agent, .delegation = std::move(value), .status = not_revoked()}));
    };

    auto expired = *authority;
    expired.at = now - std::chrono::hours{2};
    set_attribute(expired, "not_after", epoch_seconds(now - std::chrono::hours{1}));
    auto expired_signed = sign_delegation(std::move(expired));
    ASSERT_TRUE(expired_signed);
    auto expired_fork = fork(*expired_signed);
    ASSERT_FALSE(expired_fork);
    EXPECT_EQ(expired_fork.error().code, liquid::errc::unauthorized);

    auto future = *authority;
    future.at = now + std::chrono::hours{1};
    set_attribute(future, "not_after", epoch_seconds(now + std::chrono::hours{2}));
    auto future_signed = sign_delegation(std::move(future));
    ASSERT_TRUE(future_signed);
    auto future_fork = fork(*future_signed);
    ASSERT_FALSE(future_fork);
    EXPECT_EQ(future_fork.error().code, liquid::errc::unauthorized);

    auto unbounded = *authority;
    set_attribute(unbounded,
                  "not_after",
                  epoch_seconds(now + liquid::delegation_maximum_lifetime + std::chrono::hours{1}));
    auto unbounded_signed = sign_delegation(std::move(unbounded));
    ASSERT_TRUE(unbounded_signed);
    auto unbounded_fork = fork(*unbounded_signed);
    ASSERT_FALSE(unbounded_fork);
    EXPECT_EQ(unbounded_fork.error().code, liquid::errc::invalid_argument);

    auto undated = *authority;
    std::erase_if(undated.attributes,
                  [](const auto& attribute) { return attribute.name == "not_after"; });
    auto undated_signed = sign_delegation(std::move(undated));
    ASSERT_TRUE(undated_signed);
    auto undated_fork = fork(*undated_signed);
    ASSERT_FALSE(undated_fork);
    EXPECT_EQ(undated_fork.error().code, liquid::errc::invalid_argument);
}

// Must-never D4 (liquid#514): a delegation revoked, or expired, while its session runs stops the
// session's leases and publish, and the shared repository is left unchanged.
TEST(Collaboration, publish_and_lease_refuse_after_revocation_or_expiry) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    const auto agent = current_actor();
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;
    auto revoked = std::make_shared<std::atomic_bool>(false);
    auto status = liquid::delegation_status_resolver{
        [revoked](const liquid::attestation&,
                  const liquid::digest&) -> liquid::result<liquid::attestation_status> {
            return liquid::attestation_status{
                .revoked = revoked->load(),
                .checked_at =
                    std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
        }};
    auto forked = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = status}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto agent_session = std::move(*forked);
    ASSERT_TRUE(
        liquid::runtime::block_on(agent_session.write("src/value.cpp", bytes("int value = 2;\n"))));

    revoked->store(true);
    const auto claim = liquid::lease_claim{.path = "src/value.cpp"};
    auto leased = liquid::runtime::block_on(agent_session.lease(claim, std::chrono::seconds{30}));
    ASSERT_FALSE(leased);
    EXPECT_EQ(leased.error().code, liquid::errc::unauthorized);
    auto published = liquid::runtime::block_on(agent_session.publish());
    ASSERT_FALSE(published);
    EXPECT_EQ(published.error().code, liquid::errc::unauthorized);
    auto unchanged = liquid::runtime::block_on(shared.read("src/value.cpp"));
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(text(*unchanged), "int value = 1;\n");
    EXPECT_TRUE(liquid::runtime::block_on(agent_session.discard()));

    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    auto short_lived = *authority;
    set_attribute(short_lived, "not_after", epoch_seconds(now + std::chrono::seconds{2}));
    auto short_signed = sign_delegation(std::move(short_lived));
    ASSERT_TRUE(short_signed);
    auto expiring_fork = liquid::runtime::block_on(shared.fork_session(
        {.actor = agent, .delegation = *short_signed, .status = not_revoked()}));
    ASSERT_TRUE(expiring_fork) << expiring_fork.error().message;
    auto expiring = std::move(*expiring_fork);
    ASSERT_TRUE(
        liquid::runtime::block_on(expiring.write("src/value.cpp", bytes("int value = 3;\n"))));
    std::this_thread::sleep_for(std::chrono::seconds{3});
    auto late = liquid::runtime::block_on(expiring.publish());
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code, liquid::errc::unauthorized);
    auto still = liquid::runtime::block_on(shared.read("src/value.cpp"));
    ASSERT_TRUE(still);
    EXPECT_EQ(text(*still), "int value = 1;\n");
    EXPECT_TRUE(liquid::runtime::block_on(expiring.discard()));
}

// Must-nevers D8 and D9 (liquid#515): a delegation starts a session only for the agent it names,
// when its dispatcher is its issuer, and in the repository it was issued for; the session's work
// is then attributed to that agent, not to the principal.
TEST(Collaboration, fork_session_binds_the_agent_dispatcher_and_repository) {
    collaboration_directory directory;
    auto shared = forked_repository(directory.path() / "shared");
    auto other = forked_repository(directory.path() / "other");
    const auto principal = current_actor();
    const auto agent = liquid::actor{.taap_id = "taap:1:ed25519:delegated-agent"};
    auto authority = delegation(shared, agent, {"src/**"});
    ASSERT_TRUE(authority) << authority.error().message;

    const auto impostor = liquid::actor{.taap_id = "taap:1:ed25519:another-agent"};
    auto as_impostor = liquid::runtime::block_on(shared.fork_session(
        {.actor = impostor, .delegation = *authority, .status = not_revoked()}));
    ASSERT_FALSE(as_impostor);
    EXPECT_EQ(as_impostor.error().code, liquid::errc::unauthorized);
    auto as_principal = liquid::runtime::block_on(shared.fork_session(
        {.actor = principal, .delegation = *authority, .status = not_revoked()}));
    ASSERT_FALSE(as_principal);
    EXPECT_EQ(as_principal.error().code, liquid::errc::unauthorized);

    auto misdispatched = *authority;
    set_attribute(misdispatched, "dispatcher", std::string{"taap:1:ed25519:someone-else"});
    auto misdispatched_signed = sign_delegation(std::move(misdispatched));
    ASSERT_TRUE(misdispatched_signed);
    auto dispatched = liquid::runtime::block_on(shared.fork_session(
        {.actor = agent, .delegation = *misdispatched_signed, .status = not_revoked()}));
    ASSERT_FALSE(dispatched);
    EXPECT_EQ(dispatched.error().code, liquid::errc::unauthorized);

    auto elsewhere = liquid::runtime::block_on(
        other.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_FALSE(elsewhere);
    EXPECT_EQ(elsewhere.error().code, liquid::errc::unauthorized);

    auto forked = liquid::runtime::block_on(
        shared.fork_session({.actor = agent, .delegation = *authority, .status = not_revoked()}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto agent_session = std::move(*forked);
    ASSERT_TRUE(
        liquid::runtime::block_on(agent_session.write("src/value.cpp", bytes("int value = 2;\n"))));
    ASSERT_TRUE(liquid::runtime::block_on(agent_session.publish({.message = "agent"})));
    auto drips = liquid::runtime::block_on(shared.drips());
    ASSERT_TRUE(drips);
    ASSERT_FALSE(drips->empty());
    EXPECT_EQ(drips->back().by.taap_id, agent.taap_id);
    EXPECT_NE(drips->back().by.taap_id, principal.taap_id);
    EXPECT_TRUE(drips->back().delegation.has_value());
}

} // namespace
