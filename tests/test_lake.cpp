#include <gtest/gtest.h>

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <format>
#include <fstream>
#include <iterator>
#include <string>
#include <string_view>
#include <system_error>

#include "liquid/lake.hpp"
#include "liquid/repository.hpp"
#include "liquid/runtime.hpp"
#include "liquid/sync.hpp"
#include "liquid/version.hpp"
#include "support/forge_double.hpp"
#include "temporary_directory.hpp"

namespace {

class scratch {
public:
    scratch() : path_(liquid::test::fixture_path("lake-" + std::to_string(next_++))) {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
        std::filesystem::create_directories(path_, ignored);
    }
    ~scratch() {
        auto ignored = std::error_code{};
        std::filesystem::remove_all(path_, ignored);
    }
    scratch(const scratch&) = delete;
    scratch& operator=(const scratch&) = delete;

    [[nodiscard]] auto path() const -> const std::filesystem::path& {
        return path_;
    }

private:
    inline static std::atomic_uint64_t next_{};
    std::filesystem::path path_;
};

// The lake's base URL: the double serves `<base>/v1/repositories/test`.
auto base_of(const liquid::test::forge_double& server) -> std::string {
    auto url = server.url();
    return url.substr(0, url.find("/v1/"));
}

auto registry(const scratch& home, std::shared_ptr<liquid::lake_credential_store> credentials)
    -> liquid::lake_registry {
    auto opened = liquid::runtime::block_on(liquid::lake_registry::open(
        {.path = home.path() / "lakes.json", .credentials = std::move(credentials)}));
    EXPECT_TRUE(opened) << (opened ? "" : opened.error().message);
    return std::move(*opened);
}

auto compatible_info() -> std::string {
    return std::format(R"({{"api":"v1","name":"lake","version":"0.9.0","revision":"abc123",)"
                       R"("sync_protocol_versions":[1],"forge_api_versions":[1],)"
                       R"("liquid_versions":{{"oldest":"0.1.0","newest":"{}"}}}})",
                       liquid::version_string());
}

auto pull_with(const liquid::lake_target& target, liquid::repository& destination)
    -> liquid::result<liquid::sync_report> {
    auto options = liquid::sync_endpoint_options{};
    options.bearer_token = target.token.value;
    auto endpoint = liquid::sync_endpoint{target.repository_url, std::move(options)};
    return liquid::runtime::block_on(endpoint.pull(destination));
}

} // namespace

TEST(Lake, starts_with_no_lake_and_keeps_added_lakes) {
    scratch home;
    auto lakes = registry(home, liquid::memory_lake_credential_store());
    EXPECT_TRUE(lakes.list().empty());

    auto added = liquid::runtime::block_on(lakes.add("dev", "https://lake.example.invalid/"));
    ASSERT_TRUE(added) << added.error().message;
    EXPECT_EQ(added->url, "https://lake.example.invalid");
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("testing", "http://127.0.0.1:9000/prefix")));

    EXPECT_EQ(liquid::runtime::block_on(lakes.add("dev", "https://other.invalid")).error().code,
              liquid::errc::already_exists);
    EXPECT_EQ(liquid::runtime::block_on(lakes.add("again", "https://lake.example.invalid:443"))
                  .error()
                  .code,
              liquid::errc::already_exists);
    EXPECT_EQ(liquid::runtime::block_on(lakes.add("Bad Name", "https://x.invalid")).error().code,
              liquid::errc::invalid_argument);
    EXPECT_EQ(liquid::runtime::block_on(lakes.add("ftp", "ftp://x.invalid")).error().code,
              liquid::errc::invalid_argument);

    auto reopened = registry(home, liquid::memory_lake_credential_store());
    ASSERT_EQ(reopened.list().size(), 2U);
    EXPECT_EQ(reopened.find("testing")->url, "http://127.0.0.1:9000/prefix");

    EXPECT_EQ(reopened.match("https://lake.example.invalid/v1/repositories/r")->name, "dev");
    EXPECT_EQ(reopened.match("https://lake.example.invalid:443/v1/repositories/r")->name, "dev");
    EXPECT_EQ(reopened.match("http://127.0.0.1:9000/prefix/v1/repositories/r")->name, "testing");
    EXPECT_FALSE(reopened.match("http://127.0.0.1:9000/prefixed/v1/repositories/r"));
    EXPECT_FALSE(reopened.match("http://lake.example.invalid/v1/repositories/r"));

    ASSERT_TRUE(liquid::runtime::block_on(reopened.remove("dev")));
    EXPECT_FALSE(registry(home, liquid::memory_lake_credential_store()).find("dev"));
}

TEST(Lake, repository_paths_accept_an_identifier_a_path_or_a_url_on_the_lake) {
    const auto target = liquid::lake{"dev", "https://lake.example.invalid/base"};
    EXPECT_EQ(*liquid::repository_path_for(target, "abc"), "/v1/repositories/abc");
    EXPECT_EQ(*liquid::repository_path_for(target, "/v1/repositories/abc"), "/v1/repositories/abc");
    EXPECT_EQ(*liquid::repository_path_for(target,
                                           "https://lake.example.invalid/base/v1/repositories/abc"),
              "/v1/repositories/abc");
    EXPECT_FALSE(liquid::repository_path_for(target, "https://other.invalid/v1/repositories/abc"));
    EXPECT_FALSE(liquid::repository_path_for(target, "a/b"));
}

TEST(Lake, two_lakes_keep_their_own_tokens_and_a_repository_moves_between_them) {
    auto first = liquid::test::forge_double{};
    auto second = liquid::test::forge_double{};
    first.set_server_info(compatible_info());
    second.set_server_info(compatible_info());
    scratch home;
    auto credentials = liquid::memory_lake_credential_store();
    auto lakes = registry(home, credentials);
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("first", base_of(first))));
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("second", base_of(second))));
    ASSERT_TRUE(liquid::runtime::block_on(lakes.set_token("first", "token-first")));
    ASSERT_TRUE(liquid::runtime::block_on(lakes.set_token("second", "token-second")));

    scratch work;
    auto repository =
        liquid::runtime::block_on(liquid::repository::create(work.path() / "repository"));
    ASSERT_TRUE(repository);

    // No lake recorded yet: the error says how to choose one.
    auto unconfigured =
        liquid::runtime::block_on(liquid::resolve_lake_target(lakes, &*repository, ""));
    ASSERT_FALSE(unconfigured);
    EXPECT_EQ(unconfigured.error().code, liquid::errc::not_configured);
    EXPECT_NE(unconfigured.error().message.find("liquid lake use"), std::string::npos);

    // A full URL is matched to its lake by host and carries only that lake's token.
    auto by_url =
        liquid::runtime::block_on(liquid::resolve_lake_target(lakes, &*repository, first.url()));
    ASSERT_TRUE(by_url);
    EXPECT_EQ(by_url->named->name, "first");
    EXPECT_EQ(by_url->token.value, "token-first");
    EXPECT_EQ(by_url->token.source, liquid::lake_token_source::credential_store);

    ASSERT_TRUE(liquid::runtime::block_on(liquid::write_repository_lake(
        *repository, {.lake = "first", .repository = "/v1/repositories/test"})));
    auto on_first = liquid::runtime::block_on(liquid::resolve_lake_target(lakes, &*repository, ""));
    ASSERT_TRUE(on_first);
    EXPECT_EQ(on_first->repository_url, first.url());
    ASSERT_TRUE(liquid::runtime::block_on(liquid::require_compatible_lake(*on_first)));
    ASSERT_TRUE(pull_with(*on_first, *repository));
    EXPECT_EQ(first.last_authorization(), "Bearer token-first");
    EXPECT_EQ(second.last_authorization(), "");

    // The repository moves to the second lake, keeping its path there.
    ASSERT_TRUE(liquid::runtime::block_on(liquid::write_repository_lake(
        *repository, {.lake = "second", .repository = "/v1/repositories/test"})));
    auto recorded = liquid::runtime::block_on(liquid::read_repository_lake(*repository));
    ASSERT_TRUE(recorded && *recorded);
    EXPECT_EQ((*recorded)->lake, "second");
    auto on_second =
        liquid::runtime::block_on(liquid::resolve_lake_target(lakes, &*repository, ""));
    ASSERT_TRUE(on_second);
    EXPECT_EQ(on_second->repository_url, second.url());
    EXPECT_EQ(on_second->token.value, "token-second");
    static_cast<void>(pull_with(*on_second, *repository));
    EXPECT_EQ(second.last_authorization(), "Bearer token-second");
    EXPECT_EQ(first.last_authorization(), "Bearer token-first");

    // A lake named explicitly uses the repository's recorded path on that lake.
    auto named =
        liquid::runtime::block_on(liquid::resolve_lake_target(lakes, &*repository, "first"));
    ASSERT_TRUE(named);
    EXPECT_EQ(named->repository_url, first.url());
    EXPECT_EQ(named->token.value, "token-first");

    // Removing a lake erases its token from the store.
    ASSERT_TRUE(liquid::runtime::block_on(lakes.remove("first")));
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("first", base_of(first))));
    auto after = liquid::runtime::block_on(lakes.token("first"));
    ASSERT_TRUE(after);
    EXPECT_NE(after->value, "token-first");
}

TEST(Lake, environment_tokens_are_per_lake_before_the_shared_fallback) {
    scratch home;
    auto lakes = registry(home, liquid::memory_lake_credential_store());
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("my-lake", "https://one.invalid")));
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("other", "https://two.invalid")));
    ::setenv("LIQUID_TOKEN_MY_LAKE", "from-lake-variable", 1);
    ::setenv("LIQUID_TOKEN", "from-shared-variable", 1);
    auto mine = liquid::runtime::block_on(lakes.token("my-lake"));
    auto other = liquid::runtime::block_on(lakes.token("other"));
    ::unsetenv("LIQUID_TOKEN_MY_LAKE");
    ::unsetenv("LIQUID_TOKEN");
    ASSERT_TRUE(mine && other);
    EXPECT_EQ(mine->value, "from-lake-variable");
    EXPECT_EQ(mine->source, liquid::lake_token_source::lake_environment);
    EXPECT_EQ(other->value, "from-shared-variable");
    EXPECT_EQ(other->source, liquid::lake_token_source::shared_environment);

    // A stored token wins over both variables.
    ASSERT_TRUE(liquid::runtime::block_on(lakes.set_token("my-lake", "stored")));
    ::setenv("LIQUID_TOKEN_MY_LAKE", "from-lake-variable", 1);
    auto stored = liquid::runtime::block_on(lakes.token("my-lake"));
    ::unsetenv("LIQUID_TOKEN_MY_LAKE");
    ASSERT_TRUE(stored);
    EXPECT_EQ(stored->value, "stored");
}

TEST(Lake, an_unnamed_url_gets_only_the_shared_token) {
    scratch home;
    auto lakes = registry(home, liquid::memory_lake_credential_store());
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("dev", "https://one.invalid")));
    ASSERT_TRUE(liquid::runtime::block_on(lakes.set_token("dev", "dev-token")));
    auto target = liquid::runtime::block_on(
        liquid::resolve_lake_target(lakes, nullptr, "https://elsewhere.invalid/v1/repositories/x"));
    ASSERT_TRUE(target);
    EXPECT_FALSE(target->named);
    EXPECT_EQ(target->base_url, "https://elsewhere.invalid");
    EXPECT_NE(target->token.value, "dev-token");
}

TEST(Lake, refuses_a_lake_that_reports_an_unsupported_version) {
    auto server = liquid::test::forge_double{};
    server.set_server_info(
        R"({"ok":true,"value":{"api":"v1","version":"7.0.0","revision":"f00d",)"
        R"("sync_protocol_versions":[3,4],"liquid_versions":{"oldest":"6.0.0","newest":"7.0.0"}}})");
    scratch home;
    auto lakes = registry(home, liquid::memory_lake_credential_store());
    ASSERT_TRUE(liquid::runtime::block_on(lakes.add("future", base_of(server))));
    auto target =
        liquid::runtime::block_on(liquid::resolve_lake_target(lakes, nullptr, server.url()));
    ASSERT_TRUE(target);
    auto refused = liquid::runtime::block_on(liquid::require_compatible_lake(*target));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::unsupported);
    const auto& message = refused.error().message;
    EXPECT_NE(message.find("lake 'future'"), std::string::npos) << message;
    EXPECT_NE(message.find("version 7.0.0"), std::string::npos) << message;
    EXPECT_NE(message.find("sync protocol 3, 4"), std::string::npos) << message;
    EXPECT_NE(message.find(std::string{"liquid "} + std::string{liquid::version_string()} +
                           " supports sync protocol 1"),
              std::string::npos)
        << message;
    EXPECT_NE(message.find("upgrade liquid"), std::string::npos) << message;

    // The check is made once per lake for the life of the process.
    ASSERT_FALSE(liquid::runtime::block_on(liquid::require_compatible_lake(*target)));
    EXPECT_EQ(server.server_info_requests(), 1U);
}

TEST(Lake, a_lake_that_does_not_report_versions_is_allowed_with_a_warning) {
    auto server = liquid::test::forge_double{};
    auto target = liquid::lake_target{.base_url = base_of(server)};
    auto decided = liquid::runtime::block_on(liquid::require_compatible_lake(target));
    ASSERT_TRUE(decided);
    EXPECT_EQ(decided->kind, liquid::lake_compatibility_kind::unknown);
    EXPECT_NE(decided->message.find("continuing"), std::string::npos);
}

TEST(Lake, compatibility_names_the_side_that_is_behind) {
    auto info = liquid::lake_server_info{.version = "1.0.0", .sync_protocol_versions = {1}};
    EXPECT_EQ(liquid::check_lake_compatibility("dev", info).kind,
              liquid::lake_compatibility_kind::compatible);

    info.liquid_newest = "0.0.1";
    auto old_lake = liquid::check_lake_compatibility("dev", info);
    EXPECT_EQ(old_lake.kind, liquid::lake_compatibility_kind::unsupported);
    EXPECT_NE(old_lake.message.find("upgrade the lake"), std::string::npos) << old_lake.message;

    info.liquid_newest.clear();
    info.liquid_oldest = "99.0.0";
    auto old_liquid = liquid::check_lake_compatibility("dev", info);
    EXPECT_EQ(old_liquid.kind, liquid::lake_compatibility_kind::unsupported);
    EXPECT_NE(old_liquid.message.find("upgrade liquid"), std::string::npos);

    info.liquid_oldest.clear();
    info.sync_protocol_versions = {0};
    auto older_protocol = liquid::check_lake_compatibility("dev", info);
    EXPECT_EQ(older_protocol.kind, liquid::lake_compatibility_kind::unsupported);
    EXPECT_NE(older_protocol.message.find("upgrade the lake"), std::string::npos);

    EXPECT_EQ(liquid::check_lake_compatibility("dev", liquid::lake_server_info{}).kind,
              liquid::lake_compatibility_kind::unknown);
}

#if defined(LIQUID_CLI) && !defined(_WIN32)
namespace {

auto shell_word(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        output += character == '\'' ? std::string{"'\\''"} : std::string{character};
    }
    return output + "'";
}

struct cli_result {
    int status{};
    std::string output;
};

auto run_cli(const std::filesystem::path& home,
             const std::filesystem::path& directory,
             std::string_view environment,
             std::string_view arguments) -> cli_result {
    const auto captured = home / "cli-output.txt";
    const auto command = "cd " + shell_word(directory.string()) +
                         " && env LIQUID_HOME=" + shell_word(home.string()) +
                         " LIQUID_CREDENTIAL_STORE=none " + std::string{environment} + " " +
                         shell_word(LIQUID_CLI) + " " + std::string{arguments} + " > " +
                         shell_word(captured.string()) + " 2>&1";
    const auto status = std::system(command.c_str());
    auto input = std::ifstream{captured};
    return {WIFEXITED(status) ? WEXITSTATUS(status) : -1,
            std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}}};
}

} // namespace

TEST(LakeCli, clones_by_name_syncs_without_a_url_and_moves_between_lakes) {
    auto first = liquid::test::forge_double{};
    auto second = liquid::test::forge_double{};
    first.set_server_info(compatible_info());
    second.set_server_info(compatible_info());
    scratch home;
    scratch work;
    const auto tokens = std::string{"LIQUID_TOKEN_FIRST=cli-first LIQUID_TOKEN_SECOND=cli-second"};

    auto none = run_cli(home.path(), work.path(), "", "lake list");
    EXPECT_EQ(none.status, 0);
    EXPECT_NE(none.output.find("liquid lake add"), std::string::npos) << none.output;

    ASSERT_EQ(run_cli(home.path(), work.path(), "", "lake add first " + shell_word(base_of(first)))
                  .status,
              0);
    ASSERT_EQ(
        run_cli(home.path(), work.path(), "", "lake add second " + shell_word(base_of(second)))
            .status,
        0);

    auto cloned = run_cli(home.path(), work.path(), tokens, "clone --lake first test copy --json");
    ASSERT_EQ(cloned.status, 0) << cloned.output;
    EXPECT_NE(cloned.output.find("\"lake\":\"first\""), std::string::npos) << cloned.output;
    EXPECT_EQ(first.last_authorization(), "Bearer cli-first");

    const auto copy = work.path() / "copy";
    auto listed = run_cli(home.path(), copy, tokens, "lake list --json");
    ASSERT_EQ(listed.status, 0) << listed.output;
    EXPECT_NE(listed.output.find(R"("name":"first","url":")" + base_of(first) +
                                 R"(","token":"lake_environment","current":true)"),
              std::string::npos)
        << listed.output;

    auto pulled = run_cli(home.path(), copy, tokens, "pull");
    EXPECT_EQ(pulled.status, 0) << pulled.output;
    EXPECT_EQ(second.last_authorization(), "");

    auto used = run_cli(home.path(), copy, "", "lake use second");
    ASSERT_EQ(used.status, 0) << used.output;
    static_cast<void>(run_cli(home.path(), copy, tokens, "pull"));
    EXPECT_EQ(second.last_authorization(), "Bearer cli-second");

    auto checked = run_cli(home.path(), copy, "", "lake check");
    EXPECT_EQ(checked.status, 0) << checked.output;
    EXPECT_NE(checked.output.find("compatible: lake 'second'"), std::string::npos)
        << checked.output;
}

TEST(LakeCli, refuses_an_unsupported_lake_and_says_how_to_add_one) {
    auto server = liquid::test::forge_double{};
    server.set_server_info(R"({"version":"9.0.0","sync_protocol_versions":[9]})");
    scratch home;
    scratch work;
    ASSERT_EQ(run_cli(home.path(), work.path(), "", "init repository").status, 0);
    const auto repository = work.path() / "repository";

    auto unconfigured = run_cli(home.path(), repository, "", "push");
    EXPECT_EQ(unconfigured.status, 1);
    EXPECT_NE(unconfigured.output.find("liquid lake add <name> <url>"), std::string::npos)
        << unconfigured.output;

    ASSERT_EQ(run_cli(home.path(), repository, "", "lake add future " + shell_word(base_of(server)))
                  .status,
              0);
    ASSERT_EQ(run_cli(home.path(), repository, "", "lake use future test").status, 0);
    auto refused = run_cli(home.path(), repository, "LIQUID_TOKEN_FUTURE=t", "push");
    EXPECT_EQ(refused.status, 1);
    EXPECT_NE(refused.output.find("lake 'future' (version 9.0.0, sync protocol 9)"),
              std::string::npos)
        << refused.output;
    EXPECT_NE(refused.output.find("upgrade liquid"), std::string::npos) << refused.output;
    EXPECT_EQ(server.last_authorization(), "");

    auto checked = run_cli(home.path(), repository, "", "lake check future");
    EXPECT_EQ(checked.status, 1);

    static_cast<void>(run_cli(home.path(), repository, "", "daemon --shutdown"));
}
#endif
