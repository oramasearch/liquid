// SPDX-License-Identifier: MIT

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>
#include <vector>

#include "liquid/deploy.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "support/forge_double.hpp"
#include "temporary_directory.hpp"

namespace {

auto bytes(std::string_view value) -> std::vector<std::byte> {
    const auto* first = reinterpret_cast<const std::byte*>(value.data());
    return {first, first + value.size()};
}

auto image() -> std::string {
    return "registry.example.test/demo@sha256:"
           "0123456789abcdef0123456789abcdef0123456789abcdef0123456789abcdef";
}

class deployment_fixture {
public:
    deployment_fixture()
        : path_(liquid::test::fixture_path("deployment-repository-" + std::to_string(next_++))) {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
        auto opened = liquid::runtime::block_on(liquid::repository::create(path_));
        EXPECT_TRUE(opened);
        repository_ = std::move(*opened);
        EXPECT_TRUE(liquid::runtime::block_on(repository_.write("app.txt", bytes("version one"))));
        EXPECT_TRUE(liquid::runtime::block_on(repository_.cut("deployment source")));
    }

    ~deployment_fixture() {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }

    liquid::repository repository_;

    auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

} // namespace

#if defined(LIQUID_CLI) && !defined(_WIN32)
auto shell_quote(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        if (character == '\'') {
            output += "'\\''";
        } else {
            output += character;
        }
    }
    output += '\'';
    return output;
}

auto read_file(const std::filesystem::path& path) -> std::string {
    auto input = std::ifstream{path};
    return {std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto repository_name(const liquid::repository& source) -> std::string {
    const auto id = source.id();
    EXPECT_TRUE(id);
    auto output = std::string{};
    for (const auto byte : *id) {
        output += std::format("{:02x}", std::to_integer<unsigned char>(byte));
    }
    return output;
}
#endif

TEST(Deploy, creates_digest_pinned_records_and_reuses_the_same_action) {
    deployment_fixture fixture;
    auto server = liquid::test::forge_double{"deploy-token"};
    const auto request =
        liquid::deploy_request{.environment = "preview-change-a", .image = image()};
    auto first = liquid::runtime::block_on(liquid::deploy::create(
        fixture.repository_, server.url(), request, {.bearer_token = "deploy-token"}));
    ASSERT_TRUE(first) << first.error().message;
    EXPECT_EQ(first->environment, "preview-change-a");
    EXPECT_EQ(first->image, image());
    EXPECT_EQ(server.last_authorization(), "Bearer deploy-token");

    auto second = liquid::runtime::block_on(liquid::deploy::create(
        fixture.repository_, server.url(), request, {.bearer_token = "deploy-token"}));
    ASSERT_TRUE(second);
    EXPECT_EQ(second->id, first->id);

    auto listed = liquid::runtime::block_on(
        liquid::deploy::list(fixture.repository_, server.url(), {.bearer_token = "deploy-token"}));
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed->size(), 1U);
    EXPECT_EQ(liquid::to_string(listed->front().change.value),
              liquid::to_string(first->change.value));
}

TEST(Deploy, rollback_and_status_use_control_plane_capabilities) {
    deployment_fixture fixture;
    auto server = liquid::test::forge_double{};
    auto created = liquid::runtime::block_on(liquid::deploy::create(
        fixture.repository_, server.url(), {.environment = "production", .image = image()}));
    ASSERT_TRUE(created) << created.error().message;

    auto rolled_back = liquid::runtime::block_on(
        liquid::deploy::rollback(fixture.repository_, server.url(), "production", created->id));
    ASSERT_TRUE(rolled_back);
    EXPECT_EQ(rolled_back->rollback_of, created->id);
    EXPECT_EQ(liquid::to_string(rolled_back->change.value),
              liquid::to_string(created->change.value));

    auto current = liquid::runtime::block_on(
        liquid::deploy::status(fixture.repository_, server.url(), "production"));
    ASSERT_TRUE(current);
    EXPECT_EQ(current->phase, "ready");
    EXPECT_TRUE(current->ready);
}

TEST(Deploy, propagates_control_plane_error_messages) {
    deployment_fixture fixture;
    auto server = liquid::test::forge_double{};
    server.fail_next("unauthorized", "operator credential rejected", "/authorization");
    auto deployed = liquid::runtime::block_on(liquid::deploy::create(
        fixture.repository_, server.url(), {.environment = "production", .image = image()}));
    ASSERT_FALSE(deployed);
    EXPECT_EQ(deployed.error().code, liquid::errc::unauthorized);
    EXPECT_EQ(deployed.error().message, "operator credential rejected");
}

TEST(Deploy, repository_configuration_persists_named_environments) {
    const auto location = liquid::test::fixture_path("deploy-config");
    auto ignored = std::error_code{};
    std::filesystem::remove_all(location, ignored);
    auto options = liquid::repository_options{};
    options.deploy = {.control_plane = "https://operator.example.test",
                      .environments = {{"preview",
                                        {.domain = "preview.example.test",
                                         .env = {{"MODE", "preview"}},
                                         .secrets = {"secret://database"}}}},
                      .on_push = liquid::deploy_on_push::preview};
    auto created = liquid::runtime::block_on(liquid::repository::create(location, options));
    ASSERT_TRUE(created);
    auto reopened = liquid::runtime::block_on(liquid::repository::open(location));
    ASSERT_TRUE(reopened);
    const auto config = reopened->deploy_configuration();
    EXPECT_EQ(config.control_plane, options.deploy.control_plane);
    EXPECT_EQ(config.on_push, liquid::deploy_on_push::preview);
    ASSERT_TRUE(config.environments.contains("preview"));
    EXPECT_EQ(config.environments.at("preview").env.at("MODE"), "preview");
    EXPECT_EQ(config.environments.at("preview").secrets,
              (std::vector<std::string>{"secret://database"}));
    std::filesystem::remove_all(location, ignored);
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
TEST(DeployCli, built_binary_pins_deployment_json) {
    deployment_fixture fixture;
    auto server = liquid::test::forge_double{"cli-token"};
    const auto output = fixture.path() / "deploy.json";
    const auto change = liquid::runtime::block_on(fixture.repository_.head());
    ASSERT_TRUE(change);
    const auto command = "cd " + shell_quote(fixture.path().string()) +
                         " && LIQUID_TOKEN=cli-token " + shell_quote(LIQUID_CLI) +
                         " deploy --control " + shell_quote(server.url()) +
                         " --env preview --image " + shell_quote(image()) + " --json > " +
                         shell_quote(output.string());
    ASSERT_EQ(std::system(command.c_str()), 0);
    EXPECT_EQ(read_file(output),
              "{\"id\":\"deployment-1\",\"repository\":\"" + repository_name(fixture.repository_) +
                  "\",\"environment\":\"preview\",\"change\":\"" +
                  liquid::to_string(change->value) + "\",\"image\":\"" + image() +
                  "\",\"result\":null,\"rollback_of\":null}\n");
}
#endif
