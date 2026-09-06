#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <optional>
#include <random>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#if !defined(_WIN32)
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#include "data/environment.hpp"
#include "data/testing.hpp"
#include "liquid/data.hpp"
#include "liquid/repository.hpp"
#include "temporary_directory.hpp"

namespace {

auto session(std::byte value) -> liquid::session_id {
    auto output = liquid::session_id{};
    output.bytes.front() = value;
    return output;
}

auto unique_token() -> std::string {
    static auto sequence = std::atomic_uint64_t{};
    auto random = std::random_device{};
    auto output = std::ostringstream{};
    output << std::hex << random() << sequence.fetch_add(1);
    return output.str();
}

#if !defined(_WIN32)

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

auto command_text(const std::vector<std::string>& arguments) -> std::string {
    auto output = std::string{};
    for (const auto& argument : arguments) {
        if (!output.empty()) {
            output += ' ';
        }
        output += shell_quote(argument);
    }
    return output;
}

auto command_status(const std::vector<std::string>& arguments, const std::filesystem::path& log)
    -> int {
    const auto command = command_text(arguments) + " >>" + shell_quote(log.string()) + " 2>&1";
    const auto status = std::system(command.c_str());
    if (status < 0 || !WIFEXITED(status)) {
        return -1;
    }
    return WEXITSTATUS(status);
}

auto command_output(const std::vector<std::string>& arguments) -> std::optional<std::string> {
    const auto command = command_text(arguments) + " 2>/dev/null";
    auto* pipe = popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::nullopt;
    }
    auto output = std::string{};
    auto buffer = std::array<char, 512>{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const auto status = pclose(pipe);
    if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::nullopt;
    }
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
        output.pop_back();
    }
    return output;
}

auto find_program(std::string_view name) -> std::optional<std::filesystem::path> {
    auto roots = std::vector<std::filesystem::path>{};
    if (const auto* configured = std::getenv("LIQUID_POSTGRES_BIN")) {
        roots.emplace_back(configured);
    }
    if (const auto* path = std::getenv("PATH")) {
        auto paths = std::stringstream{path};
        for (auto item = std::string{}; std::getline(paths, item, ':');) {
            if (!item.empty()) {
                roots.emplace_back(item);
            }
        }
    }
    roots.emplace_back("/opt/homebrew/opt/postgresql@18/bin");
    roots.emplace_back("/opt/homebrew/opt/postgresql@17/bin");
    for (auto version = 18; version >= 14; --version) {
        roots.emplace_back("/usr/lib/postgresql/" + std::to_string(version) + "/bin");
    }
    roots.emplace_back("/usr/bin");
    for (const auto& root : roots) {
        auto code = std::error_code{};
        const auto candidate = root / name;
        if (std::filesystem::is_regular_file(candidate, code) && !code) {
            return std::filesystem::canonical(candidate, code);
        }
    }
    return std::nullopt;
}

auto reserve_port() -> std::optional<unsigned short> {
    const auto descriptor = socket(AF_INET, SOCK_STREAM, 0);
    if (descriptor < 0) {
        return std::nullopt;
    }
    auto address = sockaddr_in{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(descriptor, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0) {
        close(descriptor);
        return std::nullopt;
    }
    auto size = static_cast<socklen_t>(sizeof(address));
    if (getsockname(descriptor, reinterpret_cast<sockaddr*>(&address), &size) != 0) {
        close(descriptor);
        return std::nullopt;
    }
    close(descriptor);
    return ntohs(address.sin_port);
}

auto with_database(std::string url, std::string_view database) -> std::string {
    const auto scheme = url.find("://");
    const auto slash = url.find('/', scheme == std::string::npos ? 0 : scheme + 3);
    const auto end = url.find_first_of("?#", slash == std::string::npos ? 0 : slash + 1);
    if (slash == std::string::npos) {
        return {};
    }
    return url.substr(0, slash + 1) + std::string{database} +
           (end == std::string::npos ? std::string{} : url.substr(end));
}

class postgres_fixture final {
public:
    postgres_fixture()
        : root_(liquid::test::fixture_path("data-postgres-" + unique_token())),
          log_(root_ / "postgres.log") {
        auto code = std::error_code{};
        std::filesystem::create_directories(root_, code);
        if (code) {
            reason_ = "cannot create the PostgreSQL fixture directory: " + code.message();
            return;
        }
        if (const auto* configured = std::getenv("LIQUID_TEST_POSTGRES_URL")) {
            maintenance_url_ = configured;
            ready_ = !maintenance_url_.empty();
            if (!ready_) {
                reason_ = "LIQUID_TEST_POSTGRES_URL is empty";
            }
            return;
        }
        const auto initdb = find_program("initdb");
        pg_ctl_ = find_program("pg_ctl");
        if (!initdb || !pg_ctl_ || !find_program("psql") || !find_program("createdb") ||
            !find_program("dropdb") || !find_program("pg_dump")) {
            reason_ = "PostgreSQL client and server programs are unavailable";
            return;
        }
        if (geteuid() == 0) {
            reason_ = "initdb cannot start a PostgreSQL fixture as root; set "
                      "LIQUID_TEST_POSTGRES_URL";
            return;
        }
        const auto port = reserve_port();
        if (!port) {
            reason_ = "cannot reserve a loopback port for PostgreSQL";
            return;
        }
        data_ = root_ / "data";
        if (command_status({initdb->string(),
                            "--pgdata",
                            data_.string(),
                            "--auth=trust",
                            "--username=postgres",
                            "--no-locale",
                            "--encoding=UTF8"},
                           log_) != 0) {
            reason_ = "initdb failed; see " + log_.string();
            return;
        }
        if (command_status({pg_ctl_->string(),
                            "--pgdata",
                            data_.string(),
                            "--log",
                            log_.string(),
                            "--options",
                            "-h 127.0.0.1 -p " + std::to_string(*port),
                            "--wait",
                            "start"},
                           log_) != 0) {
            reason_ = "pg_ctl failed; see " + log_.string();
            return;
        }
        started_ = true;
        maintenance_url_ = "postgresql://postgres@127.0.0.1:" + std::to_string(*port) + "/postgres";
        ready_ = true;
    }

    ~postgres_fixture() {
        for (const auto& database : databases_) {
            drop(database);
        }
        if (started_ && pg_ctl_) {
            static_cast<void>(command_status({pg_ctl_->string(),
                                              "--pgdata",
                                              data_.string(),
                                              "--wait",
                                              "stop",
                                              "--mode=immediate"},
                                             log_));
        }
    }

    postgres_fixture(const postgres_fixture&) = delete;
    auto operator=(const postgres_fixture&) -> postgres_fixture& = delete;

    [[nodiscard]] auto ready() const noexcept -> bool {
        return ready_;
    }

    [[nodiscard]] auto reason() const -> const std::string& {
        return reason_;
    }

    auto create_base() -> std::optional<std::string> {
        const auto createdb = find_program("createdb");
        if (!createdb) {
            return std::nullopt;
        }
        const auto database = "liquid_data_test_" + unique_token();
        if (command_status({createdb->string(), "--maintenance-db", maintenance_url_, database},
                           log_) != 0) {
            return std::nullopt;
        }
        databases_.push_back(database);
        return with_database(maintenance_url_, database);
    }

    auto execute(const std::string& url, std::string statement) const -> bool {
        const auto psql = find_program("psql");
        return psql && command_status({psql->string(),
                                       "--no-psqlrc",
                                       "--set",
                                       "ON_ERROR_STOP=1",
                                       "--dbname",
                                       url,
                                       "--command",
                                       std::move(statement)},
                                      log_) == 0;
    }

    auto scalar(const std::string& url, std::string statement) const -> std::optional<std::string> {
        const auto psql = find_program("psql");
        if (!psql) {
            return std::nullopt;
        }
        return command_output({psql->string(),
                               "--no-psqlrc",
                               "--tuples-only",
                               "--no-align",
                               "--set",
                               "ON_ERROR_STOP=1",
                               "--dbname",
                               url,
                               "--command",
                               std::move(statement)});
    }

private:
    void drop(const std::string& database) const {
        const auto dropdb = find_program("dropdb");
        if (dropdb) {
            static_cast<void>(command_status({dropdb->string(),
                                              "--maintenance-db",
                                              maintenance_url_,
                                              "--force",
                                              "--if-exists",
                                              database},
                                             log_));
        }
    }

    std::filesystem::path root_;
    std::filesystem::path data_;
    std::filesystem::path log_;
    std::optional<std::filesystem::path> pg_ctl_;
    std::string maintenance_url_;
    std::string reason_;
    std::vector<std::string> databases_;
    bool started_{};
    bool ready_{};
};

TEST(DataBranch, isolates_discards_promotes_and_refuses_a_changed_base) {
    auto postgres = postgres_fixture{};
    if (!postgres.ready()) {
        GTEST_SKIP() << postgres.reason();
    }
    const auto base_url = postgres.create_base();
    ASSERT_TRUE(base_url) << "createdb failed";
    ASSERT_TRUE(postgres.execute(*base_url,
                                 "CREATE TABLE entries(value text); "
                                 "INSERT INTO entries VALUES ('base')"));

    const auto repository_path = liquid::test::fixture_path("data-repository-" + unique_token());
    auto options = liquid::repository_options{};
    options.data.databases.push_back({.name = "application",
                                      .url_env = "DATABASE_URL",
                                      .base_url = *base_url,
                                      .mechanism = liquid::data_branch_mechanism::template_copy});
    auto created = liquid::runtime::block_on(liquid::repository::create(repository_path, options));
    ASSERT_TRUE(created) << created.error().message;
    auto& repository = *created;

    const auto first_session = session(std::byte{1});
    const auto second_session = session(std::byte{2});
    auto first = liquid::runtime::block_on(
        liquid::data_branch::create(repository, first_session, {.name = "application"}));
    ASSERT_TRUE(first) << first.error().message;
    auto second = liquid::runtime::block_on(
        liquid::data_branch::create(repository, second_session, {.name = "application"}));
    ASSERT_TRUE(second) << second.error().message;
    EXPECT_NE(first->url, second->url);
    auto live_status = repository.status();
    ASSERT_TRUE(live_status);
    ASSERT_EQ(live_status->data_branches.size(), 2U);
    EXPECT_EQ(postgres.scalar(*base_url, "SELECT count(*) FROM entries"), "1");
    ASSERT_TRUE(postgres.execute(first->url, "INSERT INTO entries VALUES ('first')"));
    EXPECT_EQ(postgres.scalar(first->url, "SELECT count(*) FROM entries"), "2");
    EXPECT_EQ(postgres.scalar(second->url, "SELECT count(*) FROM entries"), "1");
    EXPECT_EQ(postgres.scalar(*base_url, "SELECT count(*) FROM entries"), "1");

    auto first_environment = liquid::data_detail::session_environment(repository, first_session);
    auto second_environment = liquid::data_detail::session_environment(repository, second_session);
    ASSERT_TRUE(first_environment);
    ASSERT_TRUE(second_environment);
    EXPECT_EQ(first_environment->at("DATABASE_URL"), first->url);
    EXPECT_EQ(second_environment->at("DATABASE_URL"), second->url);
    auto first_visible = liquid::runtime::block_on(
        liquid::data_detail::testing::current_database(repository, first_session, "DATABASE_URL"));
    auto second_visible = liquid::runtime::block_on(
        liquid::data_detail::testing::current_database(repository, second_session, "DATABASE_URL"));
    ASSERT_TRUE(first_visible) << first_visible.error().message;
    ASSERT_TRUE(second_visible) << second_visible.error().message;
    EXPECT_NE(*first_visible, *second_visible);

    const auto second_url = second->url;
    auto discarded =
        liquid::runtime::block_on(liquid::data_branch::discard(repository, second->id));
    ASSERT_TRUE(discarded) << discarded.error().message;
    EXPECT_FALSE(postgres.scalar(second_url, "SELECT current_database()"));
    auto promoted = liquid::runtime::block_on(liquid::data_branch::promote(repository, first->id));
    ASSERT_TRUE(promoted) << promoted.error().message;
    EXPECT_EQ(postgres.scalar(*base_url, "SELECT count(*) FROM entries"), "2");
    auto after_promote = liquid::runtime::block_on(liquid::data_branch::list(repository));
    ASSERT_TRUE(after_promote);
    EXPECT_TRUE(after_promote->empty());

    auto conflicting = liquid::runtime::block_on(
        liquid::data_branch::create(repository, session(std::byte{3}), {.name = "application"}));
    ASSERT_TRUE(conflicting) << conflicting.error().message;
    ASSERT_TRUE(postgres.execute(*base_url, "INSERT INTO entries VALUES ('base changed')"));
    auto refused =
        liquid::runtime::block_on(liquid::data_branch::promote(repository, conflicting->id));
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, liquid::errc::conflict);
    EXPECT_EQ(refused.error().message, "base database changed after the branch point");
    auto discarded_conflict =
        liquid::runtime::block_on(liquid::data_branch::discard(repository, conflicting->id));
    ASSERT_TRUE(discarded_conflict) << discarded_conflict.error().message;

    auto reopened = liquid::runtime::block_on(liquid::repository::open(repository_path));
    ASSERT_TRUE(reopened) << reopened.error().message;
    auto reopened_branches = liquid::runtime::block_on(liquid::data_branch::list(*reopened));
    ASSERT_TRUE(reopened_branches);
    EXPECT_TRUE(reopened_branches->empty());
    auto persisted = liquid::runtime::block_on(
        liquid::data_branch::create(*reopened, session(std::byte{4}), {.name = "application"}));
    ASSERT_TRUE(persisted) << persisted.error().message;
    auto discarded_persisted =
        liquid::runtime::block_on(liquid::data_branch::discard(*reopened, persisted->id));
    ASSERT_TRUE(discarded_persisted) << discarded_persisted.error().message;
}

#else

TEST(DataBranch, isolates_discards_promotes_and_refuses_a_changed_base) {
    GTEST_SKIP() << "PostgreSQL data actions are unavailable on Windows";
}

#endif

TEST(DataBranch, automatic_mechanism_checks_reflink_support_and_cluster_eligibility) {
    const auto probe = liquid::test::fixture_path("data-reflink-" + unique_token());
    auto selected = liquid::runtime::block_on(liquid::data_detail::testing::select_mechanism(
        liquid::data_branch_mechanism::automatic, probe, true));
    ASSERT_TRUE(selected) << selected.error().message;
    EXPECT_TRUE(*selected == liquid::data_branch_mechanism::reflink ||
                *selected == liquid::data_branch_mechanism::template_copy);
    auto explicit_reflink =
        liquid::runtime::block_on(liquid::data_detail::testing::select_mechanism(
            liquid::data_branch_mechanism::reflink, probe, true));
    if (*selected == liquid::data_branch_mechanism::reflink) {
        ASSERT_TRUE(explicit_reflink);
        EXPECT_EQ(*explicit_reflink, liquid::data_branch_mechanism::reflink);
    } else {
        ASSERT_FALSE(explicit_reflink);
        EXPECT_EQ(explicit_reflink.error().code, liquid::errc::unsupported);
    }

    auto remote = liquid::runtime::block_on(liquid::data_detail::testing::select_mechanism(
        liquid::data_branch_mechanism::automatic, probe, false));
    ASSERT_TRUE(remote);
    EXPECT_EQ(*remote, liquid::data_branch_mechanism::template_copy);

    auto unavailable = liquid::runtime::block_on(liquid::data_detail::testing::select_mechanism(
        liquid::data_branch_mechanism::reflink, probe, false));
    ASSERT_FALSE(unavailable);
    EXPECT_EQ(unavailable.error().code, liquid::errc::unsupported);
}

} // namespace
