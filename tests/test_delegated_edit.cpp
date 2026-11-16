// An assistant's edit recorded as a delegated agent acting for a person (liquid#538), through the
// public libliquid API only: no header under `src/` is included, and the target links only
// `liquid::liquid`.
#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <span>
#include <string>
#include <string_view>
#include <system_error>
#include <variant>
#include <vector>

#include "liquid/attestation.hpp"
#include "liquid/edit.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/session.hpp"
#include "liquid/sync.hpp"
#include "temporary_directory.hpp"

namespace {

class delegated_directory {
public:
    delegated_directory()
        : path_(
              liquid::test::fixture_path("liquid-delegated-edit-test-" + std::to_string(next_++))) {
        auto code = std::error_code{};
        std::filesystem::remove_all(path_, code);
    }

    ~delegated_directory() {
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

auto text(std::span<const std::byte> value) -> std::string {
    return {reinterpret_cast<const char*>(value.data()), value.size()};
}

auto position(std::string_view source, std::size_t byte) -> liquid::frontend_source_position {
    auto line = std::uint64_t{1};
    auto column = std::uint64_t{1};
    for (auto index = std::size_t{}; index < byte; ++index) {
        if (source[index] == '\n') {
            ++line;
            column = 1;
        } else {
            ++column;
        }
    }
    return {byte, line, column};
}

// A replace-node edit of the first `old_text` in `source`, at `path`.
auto replace(std::string path,
             std::string_view source,
             std::string_view old_text,
             std::string new_text) -> liquid::tree_edit {
    const auto start = source.find(old_text);
    EXPECT_NE(start, std::string_view::npos);
    const auto range = liquid::frontend_source_range{position(source, start),
                                                     position(source, start + old_text.size())};
    auto fingerprint = liquid::hash(liquid::algorithm::blake3, bytes(old_text));
    EXPECT_TRUE(fingerprint) << fingerprint.error().message;
    return {.target = {.path = std::move(path),
                       .node = {.kind = "identifier",
                                .range = range,
                                .fingerprint = fingerprint.value_or(liquid::digest{})}},
            .operation = liquid::tree_edit_operation::replace_node,
            .text = std::move(new_text)};
}

// The answer a caller's revocation source gives: not revoked, checked now.
auto not_revoked() -> liquid::delegation_status_resolver {
    return [](const liquid::attestation&,
              const liquid::digest&) -> liquid::result<liquid::attestation_status> {
        return liquid::attestation_status{.revoked = false,
                                          .checked_at = std::chrono::floor<std::chrono::seconds>(
                                              std::chrono::system_clock::now())};
    };
}

auto same(const liquid::digest& left, const liquid::digest& right) -> bool {
    return liquid::to_string(left) == liquid::to_string(right);
}

TEST(DelegatedEdit, an_assistant_edit_is_published_as_the_agent_for_the_principal) {
    delegated_directory directory;
    auto created = liquid::runtime::block_on(liquid::repository::create(directory.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto shared = std::move(*created);
    const auto left = std::string{"int shared() { return 7; }\n"};
    const auto right = std::string{"int shared();\nint use() { return shared(); }\n"};
    ASSERT_TRUE(liquid::runtime::block_on(shared.write("src/left.cpp", bytes(left))));
    ASSERT_TRUE(liquid::runtime::block_on(shared.write("src/right.cpp", bytes(right))));
    auto initial = liquid::runtime::block_on(shared.cut("initial"));
    ASSERT_TRUE(initial) << initial.error().message;

    auto principal = shared.principal();
    ASSERT_TRUE(principal) << principal.error().message;
    auto assistant = liquid::local_agent(*principal, "assistant");
    ASSERT_TRUE(assistant) << assistant.error().message;
    EXPECT_EQ(assistant->taap_id, principal->taap_id + "/agent/assistant");

    auto issued = liquid::runtime::block_on(
        shared.issue_delegation({.agent = *assistant,
                                 .scopes = {"src/left.cpp", "src/right.cpp"},
                                 .may_publish = true,
                                 .model = "editor-assistant"}));
    ASSERT_TRUE(issued) << issued.error().message;
    EXPECT_EQ(issued->delegation.profile, "delegation/1");
    EXPECT_EQ(issued->delegation.issuer.taap_id, principal->taap_id);
    auto subject = liquid::delegation_subject(*assistant);
    ASSERT_TRUE(subject);
    EXPECT_TRUE(same(issued->delegation.subject, *subject));
    EXPECT_TRUE(liquid::validate_delegation(issued->delegation));
    const auto now = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now());
    EXPECT_TRUE(liquid::check_delegation_window(issued->delegation, now));
    auto verified = liquid::verify_attestation(
        issued->delegation,
        {.now = now, .status = liquid::attestation_status{.revoked = false, .checked_at = now}});
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;

    auto preview = liquid::runtime::block_on(shared.preview_edit(
        liquid::tree_edit_request{{replace("src/left.cpp", left, "shared", "updated"),
                                   replace("src/right.cpp", right, "shared", "updated")}},
        {.tool = "liquify", .operation = "assistant", .agent = assistant->taap_id}));
    ASSERT_TRUE(preview) << preview.error().message;
    ASSERT_EQ(preview->changes.size(), 2U);

    auto forked = liquid::runtime::block_on(shared.fork_session(
        {.actor = *assistant, .delegation = issued->delegation, .status = not_revoked()}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto session = std::move(*forked);
    ASSERT_TRUE(liquid::runtime::block_on(session.apply_edit(*preview)));
    // The edit is the only change the session holds.
    EXPECT_FALSE(liquid::runtime::block_on(session.write("src/left.cpp", bytes("other\n"))));
    EXPECT_FALSE(liquid::runtime::block_on(session.apply_edit(*preview)));
    const auto drips_before = liquid::runtime::block_on(shared.drips());
    ASSERT_TRUE(drips_before);

    auto published =
        liquid::runtime::block_on(session.publish({.message = "assistant rename", .atomic = true}));
    ASSERT_TRUE(published) << published.error().message;
    ASSERT_TRUE(published->change);
    ASSERT_TRUE(published->operation);

    auto left_after = liquid::runtime::block_on(shared.read("src/left.cpp"));
    ASSERT_TRUE(left_after);
    EXPECT_EQ(text(*left_after), "int updated() { return 7; }\n");

    // One operation, recorded for the agent, acting for the principal.
    auto operations = liquid::runtime::block_on(shared.operations());
    ASSERT_TRUE(operations);
    auto structural = std::ranges::find_if(*operations, [&](const auto& value) {
        return same(value.id.value, published->operation->value);
    });
    ASSERT_NE(structural, operations->end());
    EXPECT_EQ(structural->kind, "structural_edit");
    EXPECT_EQ(structural->from, liquid::to_string(preview->id));
    EXPECT_EQ(structural->by.taap_id, assistant->taap_id);
    EXPECT_TRUE(structural->agent);
    ASSERT_TRUE(structural->principal);
    EXPECT_EQ(structural->principal->taap_id, principal->taap_id);

    // The published drips carry the delegation, name the agent and its principal, and verify
    // under the principal's key.
    auto drips = liquid::runtime::block_on(shared.drips());
    ASSERT_TRUE(drips);
    ASSERT_GT(drips->size(), drips_before->size());
    auto page = std::vector<liquid::sync_item>(1024);
    auto exported = liquid::runtime::block_on(shared.export_drips({}, page));
    ASSERT_TRUE(exported) << exported.error().message;
    page.resize(exported->count);
    auto delegated = 0U;
    for (auto index = drips_before->size(); index < drips->size(); ++index) {
        const auto& value = (*drips)[index];
        if (!value.delegation) {
            continue;
        }
        ++delegated;
        EXPECT_TRUE(same(*value.delegation, issued->id));
        EXPECT_EQ(value.by.taap_id, assistant->taap_id);
        ASSERT_TRUE(value.principal);
        EXPECT_EQ(value.principal->taap_id, principal->taap_id);
        auto item = std::ranges::find_if(page, [&](const liquid::sync_item& candidate) {
            auto id = liquid::hash(value.id.value.alg, candidate.body);
            return candidate.kind == liquid::sync_item_kind::drip && id &&
                   same(*id, value.id.value);
        });
        ASSERT_NE(item, page.end());
        EXPECT_EQ(item->author.taap_id, principal->taap_id);
        auto signature = liquid::decode_signature(item->signature);
        ASSERT_TRUE(signature) << signature.error().message;
        auto checked = liquid::verify(*principal, "drip", item->body, *signature);
        ASSERT_TRUE(checked) << checked.error().message;
        EXPECT_TRUE(*checked);
    }
    EXPECT_GT(delegated, 0U);

    // Blame names the agent and the person it acted for.
    auto attributed =
        liquid::runtime::block_on(shared.attribution("src/left.cpp", *published->change));
    ASSERT_TRUE(attributed) << attributed.error().message;
    ASSERT_EQ(attributed->size(), 1U);
    EXPECT_TRUE(attributed->front().agent);
    EXPECT_EQ(attributed->front().by.taap_id, assistant->taap_id);
    EXPECT_EQ(attributed->front().session, session.id());
    ASSERT_TRUE(attributed->front().principal);
    EXPECT_EQ(attributed->front().principal->taap_id, principal->taap_id);

    // The record survives reopening the repository.
    auto reopened = liquid::runtime::block_on(liquid::repository::open(directory.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto persisted =
        liquid::runtime::block_on(reopened->attribution("src/right.cpp", *published->change));
    ASSERT_TRUE(persisted) << persisted.error().message;
    ASSERT_EQ(persisted->size(), 2U);
    EXPECT_EQ(persisted->at(0).text, "int updated();");
    EXPECT_TRUE(persisted->at(0).agent);
    ASSERT_TRUE(persisted->at(0).principal);
    EXPECT_EQ(persisted->at(0).principal->taap_id, principal->taap_id);
    EXPECT_FALSE(persisted->at(1).agent);
    EXPECT_FALSE(persisted->at(1).principal);

    // The edit undoes as one operation.
    auto undone = liquid::runtime::block_on(reopened->undo(*published->operation));
    ASSERT_TRUE(undone) << undone.error().message;
    left_after = liquid::runtime::block_on(reopened->read("src/left.cpp"));
    auto right_after = liquid::runtime::block_on(reopened->read("src/right.cpp"));
    ASSERT_TRUE(left_after);
    ASSERT_TRUE(right_after);
    EXPECT_EQ(text(*left_after), left);
    EXPECT_EQ(text(*right_after), right);
}

TEST(DelegatedEdit, an_edit_outside_the_delegation_scopes_is_refused_before_it_applies) {
    delegated_directory directory;
    auto created = liquid::runtime::block_on(liquid::repository::create(directory.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto shared = std::move(*created);
    const auto source = std::string{"int shared() { return 7; }\n"};
    ASSERT_TRUE(liquid::runtime::block_on(shared.write("src/left.cpp", bytes(source))));
    ASSERT_TRUE(liquid::runtime::block_on(shared.write("other/left.cpp", bytes(source))));
    ASSERT_TRUE(liquid::runtime::block_on(shared.cut("initial")));
    auto principal = shared.principal();
    ASSERT_TRUE(principal);
    auto assistant = liquid::local_agent(*principal, "assistant");
    ASSERT_TRUE(assistant);
    auto issued = liquid::runtime::block_on(
        shared.issue_delegation({.agent = *assistant, .scopes = {"src/**"}, .may_publish = true}));
    ASSERT_TRUE(issued) << issued.error().message;
    auto preview = liquid::runtime::block_on(shared.preview_edit(
        liquid::tree_edit_request{{replace("src/left.cpp", source, "shared", "updated"),
                                   replace("other/left.cpp", source, "shared", "updated")}},
        {.tool = "liquify", .operation = "assistant"}));
    ASSERT_TRUE(preview) << preview.error().message;
    auto forked = liquid::runtime::block_on(shared.fork_session(
        {.actor = *assistant, .delegation = issued->delegation, .status = not_revoked()}));
    ASSERT_TRUE(forked) << forked.error().message;
    auto refused = liquid::runtime::block_on(forked->apply_edit(*preview));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    auto unchanged = liquid::runtime::block_on(forked->read("src/left.cpp"));
    ASSERT_TRUE(unchanged);
    EXPECT_EQ(text(*unchanged), source);
}

TEST(DelegatedEdit, issue_delegation_takes_the_narrowest_defaults_and_refuses_broad_requests) {
    delegated_directory directory;
    auto created = liquid::runtime::block_on(liquid::repository::create(directory.path()));
    ASSERT_TRUE(created) << created.error().message;
    auto shared = std::move(*created);
    auto principal = shared.principal();
    ASSERT_TRUE(principal);
    auto assistant = liquid::local_agent(*principal, "assistant");
    ASSERT_TRUE(assistant);

    auto issued =
        liquid::runtime::block_on(shared.issue_delegation({.agent = *assistant, .scopes = {"a"}}));
    ASSERT_TRUE(issued) << issued.error().message;
    const auto attribute = [&](std::string_view name) -> const liquid::attestation_value* {
        const auto found = std::ranges::find(
            issued->delegation.attributes, name, &liquid::attestation_attribute::name);
        return found == issued->delegation.attributes.end() ? nullptr : &found->value;
    };
    ASSERT_NE(attribute("may_publish"), nullptr);
    EXPECT_FALSE(std::get<bool>(*attribute("may_publish")));
    EXPECT_FALSE(std::get<bool>(*attribute("may_lease")));
    EXPECT_EQ(std::get<std::uint64_t>(*attribute("depth_remaining")), 0U);
    EXPECT_EQ(std::get<std::string>(*attribute("dispatcher")), principal->taap_id);
    EXPECT_EQ(
        std::get<std::uint64_t>(*attribute("not_after")),
        static_cast<std::uint64_t>((issued->delegation.at + liquid::default_delegation_lifetime)
                                       .time_since_epoch()
                                       .count()));

    EXPECT_FALSE(
        liquid::runtime::block_on(shared.issue_delegation({.agent = *principal, .scopes = {"a"}})));
    EXPECT_FALSE(liquid::runtime::block_on(shared.issue_delegation({.agent = *assistant})));
    EXPECT_FALSE(
        liquid::runtime::block_on(shared.issue_delegation({.agent = *assistant, .scopes = {""}})));
    EXPECT_FALSE(liquid::runtime::block_on(shared.issue_delegation(
        {.agent = *assistant, .scopes = {"a"}, .lifetime = std::chrono::seconds{0}})));
    EXPECT_FALSE(liquid::runtime::block_on(shared.issue_delegation(
        {.agent = *assistant,
         .scopes = {"a"},
         .lifetime = liquid::delegation_maximum_lifetime + std::chrono::seconds{1}})));
    EXPECT_FALSE(
        liquid::runtime::block_on(shared.issue_delegation({.agent = {}, .scopes = {"a"}})));

    EXPECT_FALSE(liquid::local_agent(*principal, ""));
    EXPECT_FALSE(liquid::local_agent(*principal, "Assistant"));
    EXPECT_FALSE(liquid::local_agent(*principal, "a/b"));
    EXPECT_FALSE(liquid::local_agent(*principal, std::string(65, 'a')));
    EXPECT_FALSE(liquid::local_agent(*assistant, "nested"));
    EXPECT_FALSE(liquid::local_agent(liquid::actor{}, "assistant"));
}

} // namespace
