// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <ranges>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "build/repository_access.hpp"
#include "liquid/abi.hpp"
#include "liquid/hot_update.hpp"
#include "liquid/identity.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"
#include "temporary_directory.hpp"

namespace {

auto test_digest(unsigned char value) -> liquid::digest {
    auto output = liquid::digest{liquid::algorithm::blake3, {}};
    output.bytes.front() = std::byte{value};
    return output;
}

class hot_update_repository {
public:
    hot_update_repository()
        : path_(liquid::test::fixture_path("liquid-hot-update-" + std::to_string(next_++))) {
        auto opened = liquid::runtime::block_on(liquid::repository::create(path_));
        EXPECT_TRUE(opened) << opened.error().message;
        repository_ = std::move(*opened);
    }

    auto value() -> liquid::repository& {
        return repository_;
    }

    auto result(unsigned char value,
                std::vector<liquid::digest> abis = {},
                std::optional<liquid::digest> library = {}) -> liquid::result_id {
        auto record = liquid::action_result{
            .action = test_digest(value + 20),
            .exit = 0,
            .outputs = library
                           ? std::vector<liquid::action_output>{{"out/library", 0100755, *library}}
                           : std::vector<liquid::action_output>{},
            .stdout_output = test_digest(value + 21),
            .stderr_output = test_digest(value + 22),
            .worker = "test",
            .observed_at = std::chrono::sys_seconds{std::chrono::seconds{100}},
            .abis = std::move(abis),
        };
        auto signed_value = liquid::runtime::block_on(
            liquid::repository_detail::build_access::sign_result(repository_, record));
        EXPECT_TRUE(signed_value) << signed_value.error().message;
        auto encoded = liquid::objects::encode(record);
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto stored = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository_, std::move(*encoded)));
        EXPECT_TRUE(stored) << stored.error().message;
        return {*stored};
    }

    auto abi(liquid::digest subject, std::vector<liquid::abi_symbol> symbols) -> liquid::digest {
        auto encoded = liquid::objects::encode(liquid::abi_descriptor{
            .subject = subject,
            .binary = liquid::abi_binary_kind::elf,
            .architecture = "x86_64",
            .install_name = "libdemo.so",
            .exported_symbols = std::move(symbols),
        });
        EXPECT_TRUE(encoded) << encoded.error().message;
        auto stored = liquid::runtime::block_on(
            liquid::repository_detail::build_access::put_record(repository_, std::move(*encoded)));
        EXPECT_TRUE(stored) << stored.error().message;
        return *stored;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
    liquid::repository repository_;
};

auto unit(std::string name,
          std::string signature,
          unsigned char source,
          unsigned char output,
          liquid::frontend_unit_kind kind = liquid::frontend_unit_kind::definition)
    -> liquid::hot_update_unit {
    return {kind, std::move(name), std::move(signature), test_digest(source), test_digest(output)};
}

auto evidence(liquid::result_id result,
              unsigned char artifact,
              std::vector<liquid::hot_update_unit> units) -> liquid::hot_update_build_evidence {
    return {.result = result,
            .artifact = test_digest(artifact),
            .changed_input = "source.cpp",
            .units = std::move(units)};
}

auto record(hot_update_repository& fixture, liquid::hot_update_build_evidence value)
    -> liquid::digest {
    auto stored = liquid::runtime::block_on(
        liquid::hot_update::record_evidence(fixture.value(), std::move(value)));
    EXPECT_TRUE(stored) << stored.error().message;
    return *stored;
}

// The repository's own signing identity, as the only allowed hot-update signer (liquid#388).
auto own_signer(hot_update_repository& fixture) -> liquid::hot_update_trust {
    auto signer = liquid::repository_detail::build_access::signer(fixture.value());
    EXPECT_TRUE(signer) << signer.error().message;
    return {{signer ? signer->taap_id : std::string{}}};
}

// Switches `LIQUID_HOME` for one test and restores it afterwards, removing the homes it used.
class scoped_liquid_home {
public:
    scoped_liquid_home() {
        if (const auto* previous = std::getenv("LIQUID_HOME")) {
            previous_ = previous;
        }
    }
    scoped_liquid_home(const scoped_liquid_home&) = delete;
    auto operator=(const scoped_liquid_home&) -> scoped_liquid_home& = delete;
    ~scoped_liquid_home() {
        if (previous_) {
            setenv("LIQUID_HOME", previous_->c_str(), 1);
        } else {
            unsetenv("LIQUID_HOME");
        }
        std::error_code code;
        for (const auto& path : used_) {
            std::filesystem::remove_all(path, code);
        }
    }

    auto fresh(std::string_view name) -> std::filesystem::path {
        const auto path = liquid::test::fixture_path(std::string{name});
        std::error_code code;
        std::filesystem::remove_all(path, code);
        used_.push_back(path);
        select(path);
        return path;
    }

    static auto select(const std::filesystem::path& path) -> void {
        setenv("LIQUID_HOME", path.c_str(), 1);
    }

private:
    std::optional<std::string> previous_;
    std::vector<std::filesystem::path> used_;
};

TEST(HotUpdate, CommentEditIsTierZero) {
    auto fixture = hot_update_repository{};
    const auto old_result = fixture.result(1);
    const auto new_result = fixture.result(2);
    record(fixture, evidence(old_result, 31, {unit("answer", "int()", 41, 51)}));
    record(fixture, evidence(new_result, 32, {unit("answer", "int()", 42, 51)}));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_0);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::patched);
    EXPECT_TRUE(classified->symbols.empty());
    EXPECT_EQ(classified->evidence.size(), 2U);
    auto recorded =
        liquid::runtime::block_on(liquid::hot_update::record_outcome(fixture.value(), *classified));
    ASSERT_TRUE(recorded) << recorded.error().message;
    auto statements = liquid::runtime::block_on(fixture.value().attestations(new_result.value));
    ASSERT_TRUE(statements) << statements.error().message;
    const auto outcome = std::ranges::find(statements->records,
                                           std::string{"hot_update/1"},
                                           [](const auto& item) { return item.value.profile; });
    ASSERT_NE(outcome, statements->records.end());
    auto verified = liquid::verify_attestation(outcome->value);
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_TRUE(verified->verified) << verified->reason;
}

TEST(HotUpdate, BodyEditIsTierOne) {
    auto fixture = hot_update_repository{};
    const auto old_result = fixture.result(3);
    const auto new_result = fixture.result(4);
    record(fixture, evidence(old_result, 33, {unit("answer", "int()", 43, 53)}));
    record(fixture, evidence(new_result, 34, {unit("answer", "int()", 44, 54)}));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_1);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::patched);
    EXPECT_EQ(classified->symbols, std::vector<std::string>{"answer"});
}

TEST(HotUpdate, AddedDefinitionNeedsAbiEvidenceForTierTwo) {
    auto fixture = hot_update_repository{};
    const auto old_library = test_digest(63);
    const auto new_library = test_digest(64);
    const auto old_abi = fixture.abi(old_library,
                                     {{.name = "answer",
                                       .kind = liquid::abi_symbol_kind::function,
                                       .binding = "global",
                                       .visibility = "default"}});
    const auto new_abi = fixture.abi(new_library,
                                     {{.name = "answer",
                                       .kind = liquid::abi_symbol_kind::function,
                                       .binding = "global",
                                       .visibility = "default"},
                                      {.name = "extra",
                                       .kind = liquid::abi_symbol_kind::function,
                                       .binding = "global",
                                       .visibility = "default"}});
    const auto old_result = fixture.result(5, {old_abi}, old_library);
    const auto new_result = fixture.result(6, {new_abi}, new_library);
    record(fixture, evidence(old_result, 35, {unit("answer", "int()", 45, 55)}));
    auto next =
        evidence(new_result, 36, {unit("answer", "int()", 45, 55), unit("extra", "int()", 46, 56)});
    next.abi_descriptor = new_abi;
    record(fixture, std::move(next));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_2);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::patched);
    EXPECT_EQ(classified->symbols, std::vector<std::string>{"extra"});
    EXPECT_EQ(classified->evidence.size(), 4U);
}

TEST(HotUpdate, IncompatibleAbiEvidenceIsTierThree) {
    auto fixture = hot_update_repository{};
    const auto old_library = test_digest(67);
    const auto new_library = test_digest(68);
    const auto old_abi = fixture.abi(old_library,
                                     {{.name = "state",
                                       .kind = liquid::abi_symbol_kind::object,
                                       .size = 4,
                                       .binding = "global",
                                       .visibility = "default"}});
    const auto new_abi = fixture.abi(new_library,
                                     {{.name = "state",
                                       .kind = liquid::abi_symbol_kind::object,
                                       .size = 8,
                                       .binding = "global",
                                       .visibility = "default"},
                                      {.name = "extra",
                                       .kind = liquid::abi_symbol_kind::function,
                                       .binding = "global",
                                       .visibility = "default"}});
    const auto old_result = fixture.result(18, {old_abi}, old_library);
    const auto new_result = fixture.result(19, {new_abi}, new_library);
    record(fixture, evidence(old_result, 69, {unit("answer", "int()", 70, 71)}));
    auto next =
        evidence(new_result, 72, {unit("answer", "int()", 70, 71), unit("extra", "int()", 73, 74)});
    next.abi_descriptor = new_abi;
    record(fixture, std::move(next));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_3);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::restart);
}

TEST(HotUpdate, AddedDefinitionWithoutAbiEvidenceIsTierThree) {
    auto fixture = hot_update_repository{};
    const auto old_result = fixture.result(7);
    const auto new_result = fixture.result(8);
    record(fixture, evidence(old_result, 37, {unit("answer", "int()", 47, 57)}));
    record(fixture,
           evidence(
               new_result, 38, {unit("answer", "int()", 47, 57), unit("extra", "int()", 48, 58)}));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_3);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::restart);
}

TEST(HotUpdate, SignatureOrLayoutChangeIsTierThree) {
    auto fixture = hot_update_repository{};
    const auto old_result = fixture.result(9);
    const auto new_result = fixture.result(10);
    record(fixture, evidence(old_result, 39, {unit("answer", "int()", 49, 59)}));
    record(fixture, evidence(new_result, 40, {unit("answer", "long()", 50, 60)}));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_3);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::restart);
    EXPECT_EQ(classified->symbols, std::vector<std::string>{"answer"});
}

TEST(HotUpdate, MissingFrontendUnitsIsTierFour) {
    auto fixture = hot_update_repository{};
    const auto old_result = fixture.result(11);
    const auto new_result = fixture.result(12);
    record(fixture, evidence(old_result, 61, {}));
    record(fixture, evidence(new_result, 62, {}));

    auto classified = liquid::runtime::block_on(
        liquid::hot_update::classify(fixture.value(), old_result, new_result));

    ASSERT_TRUE(classified) << classified.error().message;
    EXPECT_EQ(classified->tier, liquid::hot_update_tier::tier_4);
    EXPECT_EQ(classified->outcome, liquid::hot_update_outcome::restart);
}

TEST(HotUpdate, SignedPatchReconstructsComposition) {
    auto fixture = hot_update_repository{};
    const auto old_result = fixture.result(13);
    const auto new_result = fixture.result(14);
    const auto plan = liquid::hot_update_plan{.supersedes = old_result,
                                              .target = new_result,
                                              .tier = liquid::hot_update_tier::tier_1,
                                              .outcome = liquid::hot_update_outcome::patched,
                                              .symbols = {"answer"}};
    auto created = liquid::runtime::block_on(
        liquid::hot_update::create_patch(fixture.value(), plan, {std::byte{1}, std::byte{2}}));
    ASSERT_TRUE(created) << created.error().message;
    const auto latest_result = fixture.result(17);
    const auto latest_plan = liquid::hot_update_plan{.supersedes = new_result,
                                                     .target = latest_result,
                                                     .tier = liquid::hot_update_tier::tier_1,
                                                     .outcome = liquid::hot_update_outcome::patched,
                                                     .symbols = {"answer"}};
    auto latest = liquid::runtime::block_on(liquid::hot_update::create_patch(
        fixture.value(), latest_plan, {std::byte{3}, std::byte{4}}));
    ASSERT_TRUE(latest) << latest.error().message;

    auto verified = liquid::runtime::block_on(
        liquid::hot_update::verify_patch(fixture.value(), created->record, own_signer(fixture)));
    ASSERT_TRUE(verified) << verified.error().message;
    EXPECT_EQ(verified->symbols, std::vector<std::string>{"answer"});

    auto status = liquid::runtime::block_on(
        liquid::hot_update::status(fixture.value(), latest_result, own_signer(fixture)));
    ASSERT_TRUE(status) << status.error().message;
    EXPECT_EQ(liquid::to_string(status->artifact.value), liquid::to_string(old_result.value));
    ASSERT_EQ(status->records.size(), 2U);
    EXPECT_EQ(liquid::to_string(status->records.front()), liquid::to_string(created->record));
    EXPECT_EQ(liquid::to_string(status->records.back()), liquid::to_string(latest->record));
    ASSERT_EQ(status->patches.size(), 2U);
    EXPECT_EQ(liquid::to_string(status->patches.front().target.value),
              liquid::to_string(new_result.value));
    EXPECT_EQ(liquid::to_string(status->patches.back().target.value),
              liquid::to_string(latest_result.value));
}

TEST(HotUpdate, UnsignedPatchIsRefused) {
    auto fixture = hot_update_repository{};
    const auto value = liquid::hot_update_patch{.supersedes = fixture.result(15),
                                                .target = fixture.result(16),
                                                .tier = liquid::hot_update_tier::tier_1,
                                                .symbols = {"answer"},
                                                .object = test_digest(70)};
    auto encoded = liquid::encode_hot_update_patch(value);
    ASSERT_TRUE(encoded) << encoded.error().message;
    auto stored = liquid::runtime::block_on(
        liquid::repository_detail::build_access::put_record(fixture.value(), *encoded));
    ASSERT_TRUE(stored) << stored.error().message;

    auto verified = liquid::runtime::block_on(
        liquid::hot_update::verify_patch(fixture.value(), *stored, own_signer(fixture)));

    ASSERT_FALSE(verified);
    EXPECT_EQ(verified.error().code, liquid::errc::unauthorized);
}

// liquid#383: a patch attested by an identity this machine has no chain for is an unknown signer,
// and the patch is refused before anything is delivered.
TEST(HotUpdate, PatchFromAnUnknownSignerIsRefused) {
    const auto* previous = std::getenv("LIQUID_HOME");
    const auto restore = previous ? std::optional<std::string>{previous} : std::nullopt;
    const auto author_home = liquid::test::fixture_path("liquid-hot-update-author-home");
    const auto receiver_home = liquid::test::fixture_path("liquid-hot-update-receiver-home");
    std::error_code code;
    std::filesystem::remove_all(author_home, code);
    std::filesystem::remove_all(receiver_home, code);
    setenv("LIQUID_HOME", author_home.c_str(), 1);
    {
        auto fixture = hot_update_repository{};
        const auto plan = liquid::hot_update_plan{.supersedes = fixture.result(18),
                                                  .target = fixture.result(19),
                                                  .tier = liquid::hot_update_tier::tier_1,
                                                  .outcome = liquid::hot_update_outcome::patched,
                                                  .symbols = {"answer"}};
        auto created = liquid::runtime::block_on(
            liquid::hot_update::create_patch(fixture.value(), plan, {std::byte{5}, std::byte{6}}));
        ASSERT_TRUE(created) << created.error().message;
        // The author is in the signer set, so the refusal below is the missing chain alone.
        const auto trust = own_signer(fixture);

        setenv("LIQUID_HOME", receiver_home.c_str(), 1);
        auto verified = liquid::runtime::block_on(
            liquid::hot_update::verify_patch(fixture.value(), created->record, trust));
        ASSERT_FALSE(verified) << "a patch signed by an unknown identity was accepted";
        EXPECT_EQ(verified.error().code, liquid::errc::unauthorized);
        EXPECT_NE(verified.error().message.find("unknown signer"), std::string::npos)
            << verified.error().message;
    }
    if (restore) {
        setenv("LIQUID_HOME", restore->c_str(), 1);
    } else {
        unsetenv("LIQUID_HOME");
    }
    std::filesystem::remove_all(author_home, code);
    std::filesystem::remove_all(receiver_home, code);
}

// liquid#388: with no hot-update signer configured, every patch is refused, including one the
// repository's own identity signed.
TEST(HotUpdate, EmptySignerSetRefusesEveryPatch) {
    auto fixture = hot_update_repository{};
    const auto plan = liquid::hot_update_plan{.supersedes = fixture.result(23),
                                              .target = fixture.result(24),
                                              .tier = liquid::hot_update_tier::tier_1,
                                              .outcome = liquid::hot_update_outcome::patched,
                                              .symbols = {"answer"}};
    auto created = liquid::runtime::block_on(
        liquid::hot_update::create_patch(fixture.value(), plan, {std::byte{7}, std::byte{8}}));
    ASSERT_TRUE(created) << created.error().message;

    auto verified = liquid::runtime::block_on(
        liquid::hot_update::verify_patch(fixture.value(), created->record, {}));
    ASSERT_FALSE(verified) << "a patch was accepted with no hot-update signer configured";
    EXPECT_EQ(verified.error().code, liquid::errc::unauthorized);
    EXPECT_NE(verified.error().message.find("no hot-update signers"), std::string::npos)
        << verified.error().message;

    auto applied = liquid::runtime::block_on(liquid::hot_update::apply(
        fixture.value(), created->record, std::filesystem::path{"unused-endpoint"}, {}));
    ASSERT_FALSE(applied);
    EXPECT_EQ(applied.error().code, liquid::errc::unauthorized);
}

// liquid#388: an identity whose chain this machine holds (for example, accepted through sync) is
// authenticated, but it may sign a patch only when it is in the hot-update signer set.
TEST(HotUpdate, PatchFromAnUnlistedSignerIsRefused) {
    auto homes = scoped_liquid_home{};
    homes.fresh("liquid-hot-update-unlisted-author-home");
    auto fixture = hot_update_repository{};
    const auto plan = liquid::hot_update_plan{.supersedes = fixture.result(25),
                                              .target = fixture.result(26),
                                              .tier = liquid::hot_update_tier::tier_1,
                                              .outcome = liquid::hot_update_outcome::patched,
                                              .symbols = {"answer"}};
    auto created = liquid::runtime::block_on(
        liquid::hot_update::create_patch(fixture.value(), plan, {std::byte{9}, std::byte{10}}));
    ASSERT_TRUE(created) << created.error().message;
    const auto author = own_signer(fixture);
    ASSERT_EQ(author.signers.size(), 1U);
    auto chain = liquid::load_identity_chain(author.signers.front());
    ASSERT_TRUE(chain) << chain.error().message;

    // The verifier holds the author's genuine chain, and has an identity of its own.
    homes.fresh("liquid-hot-update-unlisted-receiver-home");
    auto receiver = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    ASSERT_TRUE(receiver) << receiver.error().message;
    auto trusted = liquid::trust_identity_chain(*chain);
    ASSERT_TRUE(trusted) << trusted.error().message;

    const auto unlisted = liquid::hot_update_trust{{receiver->identity().taap_id}};
    auto refused = liquid::runtime::block_on(
        liquid::hot_update::verify_patch(fixture.value(), created->record, unlisted));
    ASSERT_FALSE(refused) << "a patch from an identity outside the signer set was accepted";
    EXPECT_EQ(refused.error().code, liquid::errc::unauthorized);
    EXPECT_NE(refused.error().message.find("not signed by a configured hot-update signer"),
              std::string::npos)
        << refused.error().message;

    auto status = liquid::runtime::block_on(
        liquid::hot_update::status(fixture.value(), plan.target, unlisted));
    ASSERT_FALSE(status) << "status followed a patch from an identity outside the signer set";
    EXPECT_EQ(status.error().code, liquid::errc::unauthorized);

    auto accepted = liquid::runtime::block_on(
        liquid::hot_update::verify_patch(fixture.value(), created->record, author));
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_EQ(accepted->symbols, std::vector<std::string>{"answer"});
}

} // namespace
