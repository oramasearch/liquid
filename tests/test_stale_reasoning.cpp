#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <filesystem>
#include <iterator>
#include <span>
#include <string>
#include <vector>

#include "encoding/cbor.hpp"
#include "identity/taap.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/store.hpp"
#include "liquid/sync.hpp"
#include "temporary_directory.hpp"

namespace {

class stale_repository {
public:
    stale_repository()
        : path_(liquid::test::fixture_path("liquid-stale-reasoning-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~stale_repository() {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
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

auto create_base(liquid::repository& repository) -> liquid::change_id {
    EXPECT_TRUE(liquid::runtime::block_on(repository.write("notes.txt", bytes("one\ntwo\n"))));
    auto base = liquid::runtime::block_on(repository.cut("base"));
    EXPECT_TRUE(base) << base.error().message;
    return base.value_or(liquid::change_id{});
}

auto export_all(liquid::repository& repository) -> liquid::result<std::vector<liquid::sync_item>> {
    auto output = std::vector<liquid::sync_item>{};
    auto cursor = liquid::journal_cursor{};
    for (;;) {
        auto page = std::array<liquid::sync_item, 256>{};
        auto exported = liquid::runtime::block_on(repository.export_drips(cursor, page));
        if (!exported) {
            return std::unexpected(exported.error());
        }
        output.insert(output.end(),
                      std::make_move_iterator(page.begin()),
                      std::make_move_iterator(page.begin() + exported->count));
        cursor = exported->cursor;
        if (exported->done) {
            return output;
        }
    }
}

TEST(StaleReasoning, ListedAgentsMustNameABaseAndOtherActorsNeedNot) {
    auto local_signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(local_signer) << local_signer.error().message;

    stale_repository listed_path;
    auto listed_options = liquid::repository_options{};
    listed_options.agents.require_base = true;
    listed_options.agents.actors.push_back(local_signer->identity().taap_id);
    auto listed = liquid::runtime::block_on(
        liquid::repository::create(listed_path.path(), std::move(listed_options)));
    ASSERT_TRUE(listed) << listed.error().message;
    const auto before = listed->status()->pending_drips;
    auto refused = liquid::runtime::block_on(listed->write("agent.txt", bytes("refused")));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::invalid_argument);
    EXPECT_EQ(refused.error().message, "agent writes must name the version they reasoned from");
    EXPECT_EQ(listed->status()->pending_drips, before);

    stale_repository unlisted_path;
    auto unlisted_options = liquid::repository_options{};
    unlisted_options.agents.require_base = true;
    unlisted_options.agents.actors.push_back("taap:1:ed25519:another-actor");
    auto unlisted = liquid::runtime::block_on(
        liquid::repository::create(unlisted_path.path(), std::move(unlisted_options)));
    ASSERT_TRUE(unlisted) << unlisted.error().message;
    EXPECT_TRUE(liquid::runtime::block_on(unlisted->write("person.txt", bytes("accepted"))));

    auto reopened = liquid::runtime::block_on(liquid::repository::open(unlisted_path.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    const auto status = reopened->status();
    ASSERT_TRUE(status);
    EXPECT_TRUE(status->agents.require_base);
    ASSERT_EQ(status->agents.actors.size(), 1U);
    EXPECT_EQ(status->agents.actors.front(), "taap:1:ed25519:another-actor");
}

TEST(StaleReasoning, UnmovedBaseIsRecordedInTheSignedDripProjection) {
    stale_repository path;
    auto created = liquid::runtime::block_on(liquid::repository::create(path.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto base = create_base(repository);
    const auto cursor = liquid::journal_cursor{"1"};

    auto written = liquid::runtime::block_on(repository.write(
        "agent.txt", bytes("planned\n"), {.base = liquid::write_base{base, cursor}}));
    ASSERT_TRUE(written) << written.error().message;
    auto drips = liquid::runtime::block_on(repository.drips());
    ASSERT_TRUE(drips) << drips.error().message;
    ASSERT_FALSE(drips->empty());
    const auto& recorded = drips->back();
    ASSERT_TRUE(recorded.reasoned_from);
    EXPECT_EQ(recorded.reasoned_from->change.value.bytes, base.value.bytes);
    ASSERT_TRUE(recorded.reasoned_from->cursor);
    EXPECT_EQ(recorded.reasoned_from->cursor->token, "1");
    EXPECT_FALSE(recorded.revalidated);
    ASSERT_EQ(recorded.ops.size(), 1U);
    EXPECT_EQ(recorded.ops.front().path, "agent.txt");
}

TEST(StaleReasoning, VersionOneDripBodiesRemainReadable) {
    stale_repository path;
    auto created = liquid::runtime::block_on(liquid::repository::create(path.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    auto signer = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(signer) << signer.error().message;
    using liquid::encoding::cbor::value;
    auto body = liquid::encoding::cbor::encode(value{value::map{
        {value{"v"}, value{std::uint64_t{1}}},
        {value{"kind"}, value{"drip"}},
        {value{"author"}, value{signer->identity().taap_id}},
        {value{"at"}, value{std::int64_t{1}}},
        {value{"node"}, value{value::bytes{}}},
        {value{"operation"}, value{"write"}},
    }});
    ASSERT_TRUE(body) << body.error().message;
    auto signed_value = liquid::runtime::block_on(signer->sign("drip", *body));
    ASSERT_TRUE(signed_value) << signed_value.error().message;
    auto signature = liquid::identity_detail::canonical_signature(*signed_value);
    ASSERT_TRUE(signature) << signature.error().message;
    auto items = std::array{liquid::sync_item{
        .kind = liquid::sync_item_kind::drip,
        .author = signer->identity(),
        .body = std::move(*body),
        .signature = std::move(*signature),
    }};
    auto imported = liquid::runtime::block_on(repository.import_drips(items));
    ASSERT_TRUE(imported) << imported.error().message;

    auto drips = liquid::runtime::block_on(repository.drips());
    ASSERT_TRUE(drips) << drips.error().message;
    ASSERT_EQ(drips->size(), 1U);
    EXPECT_TRUE(drips->back().ops.empty());
    EXPECT_FALSE(drips->back().reasoned_from);
    EXPECT_FALSE(drips->back().revalidated);
}

TEST(StaleReasoning, SamePathMovementIsRefusedAndExplainedWithTextHunks) {
    stale_repository path;
    auto created = liquid::runtime::block_on(liquid::repository::create(path.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto base = create_base(repository);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.txt", bytes("one\nchanged\n"))));
    const auto before = repository.status()->pending_drips;

    auto refused = liquid::runtime::block_on(
        repository.write("notes.txt", bytes("agent\n"), {.base = liquid::write_base{base}}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::conflict);
    EXPECT_EQ(repository.status()->pending_drips, before);

    auto report = liquid::runtime::block_on(
        repository.explain_stale(liquid::write_base{base}, {"notes.txt"}));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_EQ(report->since_drips, 1U);
    ASSERT_EQ(report->changed.size(), 1U);
    EXPECT_EQ(report->changed.front().path, "notes.txt");
    ASSERT_EQ(report->changed.front().hunks.size(), 1U);
    EXPECT_EQ(report->changed.front().hunks.front().old_line, 2U);
    EXPECT_EQ(report->changed.front().hunks.front().new_line, 2U);
}

TEST(StaleReasoning, DisjointRevalidationIncludesDeclaredDependencies) {
    stale_repository accepted_path;
    auto accepted_created =
        liquid::runtime::block_on(liquid::repository::create(accepted_path.path()));
    ASSERT_TRUE(accepted_created) << accepted_created.error().message;
    auto accepted = std::move(*accepted_created);
    const auto accepted_base = create_base(accepted);
    ASSERT_TRUE(liquid::runtime::block_on(accepted.write("readme.txt", bytes("human\n"))));
    auto accepted_write = liquid::runtime::block_on(
        accepted.write("agent.txt",
                       bytes("agent\n"),
                       {.base = liquid::write_base{accepted_base},
                        .on_stale = liquid::write_options::stale_action::accept_if_disjoint}));
    ASSERT_TRUE(accepted_write) << accepted_write.error().message;
    auto accepted_drips = liquid::runtime::block_on(accepted.drips());
    ASSERT_TRUE(accepted_drips);
    ASSERT_TRUE(accepted_drips->back().revalidated);
    EXPECT_EQ(*accepted_drips->back().revalidated, liquid::stale_revalidation::disjoint);

    stale_repository refused_path;
    auto refused_created =
        liquid::runtime::block_on(liquid::repository::create(refused_path.path()));
    ASSERT_TRUE(refused_created) << refused_created.error().message;
    auto refused = std::move(*refused_created);
    const auto refused_base = create_base(refused);
    ASSERT_TRUE(liquid::runtime::block_on(refused.write("readme.txt", bytes("human\n"))));
    auto refused_write = liquid::runtime::block_on(
        refused.write("agent.txt",
                      bytes("agent\n"),
                      {.base = liquid::write_base{refused_base},
                       .depends_on = {"readme.txt"},
                       .on_stale = liquid::write_options::stale_action::accept_if_disjoint}));
    ASSERT_FALSE(refused_write);
    EXPECT_EQ(refused_write.error().code, liquid::errc::conflict);
    EXPECT_EQ(refused_write.error().context, "readme.txt");
}

TEST(StaleReasoning, ForcedWriteLandsAndCarriesItsRevalidation) {
    stale_repository path;
    auto created = liquid::runtime::block_on(liquid::repository::create(path.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto repository = std::move(*created);
    const auto base = create_base(repository);
    ASSERT_TRUE(liquid::runtime::block_on(repository.write("notes.txt", bytes("one\nhuman\n"))));

    auto forced = liquid::runtime::block_on(
        repository.write("notes.txt",
                         bytes("one\nreplanned\n"),
                         {.base = liquid::write_base{base},
                          .on_stale = liquid::write_options::stale_action::force}));
    ASSERT_TRUE(forced) << forced.error().message;
    auto drips = liquid::runtime::block_on(repository.drips());
    ASSERT_TRUE(drips);
    ASSERT_TRUE(drips->back().revalidated);
    EXPECT_EQ(*drips->back().revalidated, liquid::stale_revalidation::forced);
}

TEST(StaleReasoning, SyncedReplicaMovementIsRefusedWithTheRemoteHunk) {
    stale_repository first_path;
    stale_repository second_path;
    auto opened_store = liquid::runtime::block_on(liquid::store::open(first_path.path() / "store"));
    ASSERT_TRUE(opened_store) << opened_store.error().message;
    auto shared_store = std::move(*opened_store);
    auto options = liquid::repository_options{};
    options.store = &shared_store;
    auto first_created =
        liquid::runtime::block_on(liquid::repository::create(first_path.path() / "first", options));
    ASSERT_TRUE(first_created) << first_created.error().message;
    auto first = std::move(*first_created);
    const auto base = create_base(first);
    auto second_created = liquid::runtime::block_on(
        liquid::repository::create(second_path.path() / "second", options));
    ASSERT_TRUE(second_created) << second_created.error().message;
    auto second = std::move(*second_created);
    auto initial_sync = export_all(first);
    ASSERT_TRUE(initial_sync) << initial_sync.error().message;
    auto initial_import = liquid::runtime::block_on(second.import_drips(*initial_sync));
    ASSERT_TRUE(initial_import) << initial_import.error().message;
    ASSERT_TRUE(liquid::runtime::block_on(
        first.write("agent-a.txt", bytes("local plan\n"), {.base = liquid::write_base{base}})));
    ASSERT_TRUE(liquid::runtime::block_on(
        second.write("notes.txt", bytes("one\nremote\n"), {.base = liquid::write_base{base}})));

    auto remote_sync = export_all(second);
    ASSERT_TRUE(remote_sync) << remote_sync.error().message;
    auto imported = liquid::runtime::block_on(first.import_drips(*remote_sync));
    ASSERT_TRUE(imported) << imported.error().message;
    auto refused = liquid::runtime::block_on(
        first.write("notes.txt", bytes("one\nlocal\n"), {.base = liquid::write_base{base}}));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::conflict);
    auto report =
        liquid::runtime::block_on(first.explain_stale(liquid::write_base{base}, {"notes.txt"}));
    ASSERT_TRUE(report) << report.error().message;
    ASSERT_EQ(report->changed.size(), 1U);
    ASSERT_EQ(report->changed.front().hunks.size(), 1U);
    EXPECT_EQ(report->changed.front().hunks.front().old_line, 2U);
}

} // namespace
