#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <ranges>
#include <string>
#include <string_view>
#include <vector>

#include "build/cache.hpp"
#include "identity/taap.hpp"
#include "liquid/attestation.hpp"
#include "liquid/build.hpp"
#include "liquid/repository.hpp"
#include "objects/records.hpp"

namespace {

class temporary_directory {
public:
    explicit temporary_directory(std::string_view label)
        : path_(std::filesystem::temp_directory_path() /
                ("liquid-policy-test-" + std::string{label} + "-" + std::to_string(next_++))) {
        std::error_code code;
        std::filesystem::remove_all(path_, code);
    }

    ~temporary_directory() {
        std::error_code code;
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

auto host_platform() -> liquid::platform {
#if defined(__linux__)
#if defined(__aarch64__)
    return {"linux", "arm64"};
#else
    return {"linux", "x86_64"};
#endif
#elif defined(__APPLE__)
    return {"macos", "arm64"};
#else
    return {"windows", "x86_64"};
#endif
}

auto signer_at(const std::filesystem::path& home) -> liquid::signer {
    setenv("LIQUID_HOME", home.c_str(), 1);
    auto opened = liquid::runtime::block_on(liquid::identity_detail::local_signer());
    EXPECT_TRUE(opened) << opened.error().message;
    return std::move(*opened);
}

auto signed_reproducibility(liquid::digest subject, liquid::signer& signer) -> liquid::attestation {
    auto value = liquid::attestation{
        .subject = subject,
        .profile = "reproducibility/1",
        .at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now()),
        .issuer = signer.identity(),
        .subject_kind = liquid::attestation_subject_kind::artifact,
        .attributes = {{"verified_by", signer.identity().taap_id},
                       {"sandbox", "blake3:policy-test"},
                       {"agreement", true}}};
    auto body = liquid::objects::encode_attestation_body(value);
    EXPECT_TRUE(body) << body.error().message;
    auto signature = liquid::runtime::block_on(signer.sign("attestation", *body));
    EXPECT_TRUE(signature) << signature.error().message;
    auto encoded_signature = liquid::encode_attestation_signature(*signature);
    EXPECT_TRUE(encoded_signature) << encoded_signature.error().message;
    value.signature = std::move(*encoded_signature);
    return value;
}

auto store_attestation(liquid::repository& repository, liquid::attestation value)
    -> liquid::digest {
    auto stored = liquid::runtime::block_on(repository.store_attestation(std::move(value)));
    EXPECT_TRUE(stored) << stored.error().message;
    return *stored;
}

auto path_policy(std::vector<std::string> authorities,
                 std::uint32_t count,
                 liquid::policy_on_missing on_missing = liquid::policy_on_missing::refuse)
    -> liquid::policy_config {
    return {.rules = {{.scope = liquid::policy_scope::paths,
                       .paths = {"input.txt"},
                       .require = {{.profile = "reproducibility/1",
                                    .from = std::move(authorities),
                                    .min_count = count}},
                       .on_missing = on_missing}}};
}

struct policy_fixture {
    temporary_directory location{"repository"};
    temporary_directory owner_home{"owner"};
    liquid::repository repository;
    liquid::digest input_blob;
    liquid::digest output_blob;
    liquid::digest root;
    liquid::digest toolchain;

    policy_fixture() {
        auto owner = signer_at(owner_home.path());
        auto options = liquid::repository_options{};
        options.cache.publish = true;
        auto created =
            liquid::runtime::block_on(liquid::repository::create(location.path(), options));
        EXPECT_TRUE(created) << created.error().message;
        repository = std::move(*created);
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("input.txt", bytes("input"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository.write("output.txt", {})));
        EXPECT_TRUE(liquid::runtime::block_on(repository.cut("policy fixture")));
        auto tree = liquid::runtime::block_on(repository.root());
        EXPECT_TRUE(tree) << tree.error().message;
        const auto input = std::ranges::find(tree->entries, "input.txt", &liquid::tree_entry::name);
        const auto output =
            std::ranges::find(tree->entries, "output.txt", &liquid::tree_entry::name);
        if (input == tree->entries.end() || output == tree->entries.end()) {
            ADD_FAILURE() << "policy fixture blobs are missing";
            return;
        }
        input_blob = input->ref;
        output_blob = output->ref;
        auto log = liquid::runtime::block_on(repository.log());
        EXPECT_TRUE(log) << log.error().message;
        root = log->front().root;
        toolchain = root;
    }

    auto action() const -> liquid::action_input {
        return {.command = {"unused"},
                .cwd = "",
                .env = {},
                .outputs = {"output.txt"},
                .inputs = root,
                .toolchain = toolchain,
                .platform = host_platform()};
    }
};

TEST(BuildPolicy, missing_attestation_refuses_or_warns_as_configured) {
    policy_fixture fixture;
    auto builder = signer_at(fixture.location.path() / "builder");
    ASSERT_TRUE(liquid::runtime::block_on(
        fixture.repository.set_policy(path_policy({builder.identity().taap_id}, 1))));
    auto refused = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(refused) << refused.error().message;
    EXPECT_FALSE(refused->allowed);
    ASSERT_EQ(refused->inputs.size(), 1U);
    EXPECT_EQ(refused->inputs.front().input, "input.txt");
    EXPECT_FALSE(refused->inputs.front().satisfied);

    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.set_policy(
        path_policy({builder.identity().taap_id}, 1, liquid::policy_on_missing::warn))));
    auto warned = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(warned) << warned.error().message;
    EXPECT_TRUE(warned->allowed);
    ASSERT_EQ(warned->warnings.size(), 1U);
    EXPECT_NE(warned->warnings.front().find("input.txt"), std::string::npos);
}

TEST(BuildPolicy, attestation_from_an_unapproved_builder_is_refused) {
    policy_fixture fixture;
    auto approved = signer_at(fixture.location.path() / "approved");
    auto unapproved = signer_at(fixture.location.path() / "unapproved");
    auto policy = path_policy({"approved-builders"}, 1);
    policy.approved_builders = {approved.identity().taap_id};
    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.set_policy(std::move(policy))));
    store_attestation(fixture.repository, signed_reproducibility(fixture.input_blob, unapproved));
    auto report = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_FALSE(report->allowed);
    EXPECT_EQ(report->inputs.front().requirements.front().count, 0U);
}

TEST(BuildPolicy, invalid_signature_never_satisfies_a_requirement) {
    policy_fixture fixture;
    auto builder = signer_at(fixture.location.path() / "invalid-signature-builder");
    ASSERT_TRUE(liquid::runtime::block_on(
        fixture.repository.set_policy(path_policy({builder.identity().taap_id}, 1))));
    auto attestation = signed_reproducibility(fixture.input_blob, builder);
    ASSERT_FALSE(attestation.signature.empty());
    attestation.signature.back() ^= std::byte{1};
    store_attestation(fixture.repository, std::move(attestation));
    auto report = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_FALSE(report->allowed);
    EXPECT_EQ(report->inputs.front().requirements.front().count, 0U);
    EXPECT_NE(report->inputs.front().requirements.front().reason.find("rejection"),
              std::string::npos);
}

TEST(BuildPolicy, unknown_attestation_profile_is_rejected_at_configuration_time) {
    policy_fixture fixture;
    auto builder = signer_at(fixture.location.path() / "unknown-profile-builder");
    auto policy = path_policy({builder.identity().taap_id}, 1);
    policy.rules.front().require.front().profile = "unknown/1";
    auto configured = liquid::runtime::block_on(fixture.repository.set_policy(std::move(policy)));
    ASSERT_FALSE(configured);
    EXPECT_EQ(configured.error().code, liquid::errc::invalid_argument);
    EXPECT_NE(configured.error().message.find("unknown attestation profile"), std::string::npos);
}

TEST(BuildPolicy, reproducibility_quorum_counts_distinct_verified_identities) {
    policy_fixture fixture;
    auto first = signer_at(fixture.location.path() / "first");
    auto second = signer_at(fixture.location.path() / "second");
    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.set_policy(
        path_policy({first.identity().taap_id, second.identity().taap_id}, 2))));
    store_attestation(fixture.repository, signed_reproducibility(fixture.input_blob, first));
    auto one = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(one) << one.error().message;
    EXPECT_FALSE(one->allowed);
    EXPECT_EQ(one->inputs.front().requirements.front().count, 1U);

    store_attestation(fixture.repository, signed_reproducibility(fixture.input_blob, second));
    auto two = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(two) << two.error().message;
    EXPECT_TRUE(two->allowed);
    EXPECT_EQ(two->inputs.front().requirements.front().count, 2U);
}

TEST(BuildPolicy, toolchain_tree_is_evaluated_like_the_source_tree) {
    policy_fixture fixture;
    auto builder = signer_at(fixture.location.path() / "tool-builder");
    auto policy =
        liquid::policy_config{.rules = {{.scope = liquid::policy_scope::toolchain,
                                         .require = {{.profile = "reproducibility/1",
                                                      .from = {builder.identity().taap_id},
                                                      .min_count = 1}}}}};
    ASSERT_TRUE(liquid::runtime::block_on(fixture.repository.set_policy(std::move(policy))));
    auto missing = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(missing) << missing.error().message;
    EXPECT_FALSE(missing->allowed);
    ASSERT_EQ(missing->inputs.size(), 2U);
    EXPECT_EQ(missing->inputs.front().input, "toolchain:input.txt")
        << missing->inputs.front().requirements.front().reason;

    store_attestation(fixture.repository, signed_reproducibility(fixture.input_blob, builder));
    store_attestation(fixture.repository, signed_reproducibility(fixture.output_blob, builder));
    auto accepted = liquid::runtime::block_on(
        liquid::build::check_policy(fixture.repository, fixture.action()));
    ASSERT_TRUE(accepted) << accepted.error().message;
    EXPECT_TRUE(accepted->allowed);
}

TEST(BuildPolicy, subject_index_rebuilds_when_the_repository_reopens) {
    policy_fixture fixture;
    auto builder = signer_at(fixture.location.path() / "reopen-builder");
    ASSERT_TRUE(liquid::runtime::block_on(
        fixture.repository.set_policy(path_policy({builder.identity().taap_id}, 1))));
    store_attestation(fixture.repository, signed_reproducibility(fixture.input_blob, builder));
    const auto action = fixture.action();
    fixture.repository = {};
    auto reopened = liquid::runtime::block_on(liquid::repository::open(fixture.location.path()));
    ASSERT_TRUE(reopened) << reopened.error().message;
    fixture.repository = std::move(*reopened);
    auto report =
        liquid::runtime::block_on(liquid::build::check_policy(fixture.repository, action));
    ASSERT_TRUE(report) << report.error().message;
    EXPECT_TRUE(report->allowed);
}

TEST(BuildPolicy, policy_digest_changes_the_cache_candidate) {
    policy_fixture fixture;
    auto cache = liquid::build_detail::disk_cache{fixture.repository};
    auto first = cache.candidate_key(fixture.action(), fixture.toolchain);
    ASSERT_TRUE(first) << first.error().message;
    auto builder = signer_at(fixture.location.path() / "cache-builder");
    ASSERT_TRUE(liquid::runtime::block_on(
        fixture.repository.set_policy(path_policy({builder.identity().taap_id}, 1))));
    auto second = cache.candidate_key(fixture.action(), fixture.toolchain);
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(liquid::to_string(*first), liquid::to_string(*second));
}

#if LIQUID_BUILD_HAS_FUSE
TEST(BuildPolicy, refusal_happens_before_the_action_spawns) {
    policy_fixture fixture;
    auto builder = signer_at(fixture.location.path() / "execution-builder");
    ASSERT_TRUE(liquid::runtime::block_on(
        fixture.repository.set_policy(path_policy({builder.identity().taap_id}, 1))));
    liquid::build_detail::testing::reset_sandbox_spawn_count();
    auto result =
        liquid::runtime::block_on(liquid::build::execute(fixture.repository, fixture.action()));
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, liquid::errc::unauthorized);
    EXPECT_NE(result.error().message.find("input.txt"), std::string::npos);
    EXPECT_NE(result.error().message.find("config.policy.rules[0]"), std::string::npos);
    EXPECT_EQ(liquid::build_detail::testing::sandbox_spawn_count(), 0U);
}

#endif

} // namespace
