#include "liquid/data.hpp"

#include <algorithm>
#include <array>
#include <atomic>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <random>
#include <set>
#include <sstream>
#include <string_view>
#include <utility>

#if !defined(_WIN32)
#include <fcntl.h>
#include <sys/file.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

#if defined(__APPLE__)
#include <sys/clonefile.h>
#elif defined(__linux__)
#include <linux/fs.h>
#include <sys/ioctl.h>
#endif

#include "data/environment.hpp"
#include "data/repository_access.hpp"
#include "data/testing.hpp"
#include "liquid/hash.hpp"
#include "sandbox/sandbox.hpp"

namespace liquid {
namespace {

struct postgres_url {
    std::string text;
    std::size_t database_begin{};
    std::size_t database_end{};
    std::string database;

    [[nodiscard]] auto with_database(std::string_view name) const -> std::string {
        return text.substr(0, database_begin) + std::string{name} + text.substr(database_end);
    }
};

struct action_output {
    int exit{};
    std::string output;
    std::string diagnostic;
};

struct postgres_clients {
    std::filesystem::path psql;
    std::filesystem::path createdb;
    std::filesystem::path dropdb;
    std::filesystem::path pg_dump;
};

struct data_action_record {
    std::filesystem::path executable;
    std::vector<std::string> arguments;
    std::map<std::string, std::string> environment;
};

class operation_lock final {
public:
    operation_lock() = default;

    explicit operation_lock(int descriptor) : descriptor_(descriptor) {}

    ~operation_lock() {
        release();
    }

    operation_lock(operation_lock&& other) noexcept
        : descriptor_(std::exchange(other.descriptor_, -1)) {}

    auto operator=(operation_lock&& other) noexcept -> operation_lock& {
        if (this != &other) {
            release();
            descriptor_ = std::exchange(other.descriptor_, -1);
        }
        return *this;
    }

    operation_lock(const operation_lock&) = delete;
    auto operator=(const operation_lock&) -> operation_lock& = delete;

private:
    void release() noexcept {
#if !defined(_WIN32)
        if (descriptor_ >= 0) {
            static_cast<void>(::flock(descriptor_, LOCK_UN));
            static_cast<void>(::close(descriptor_));
        }
#endif
        descriptor_ = -1;
    }

    int descriptor_{-1};
};

auto failure(errc code, std::string message, std::string context = {}) -> error {
    return {code, std::move(message), std::move(context)};
}

auto acquire_operation_lock_blocking(const std::filesystem::path& root) -> result<operation_lock> {
#if defined(_WIN32)
    static_cast<void>(root);
    return std::unexpected(
        failure(errc::unsupported, "PostgreSQL data actions are unavailable on Windows"));
#else
    const auto directory = root / ".liquid-data";
    auto code = std::error_code{};
    std::filesystem::create_directories(directory, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), directory.string()));
    }
    const auto path = directory / "operation.lock";
    const auto descriptor = ::open(path.c_str(), O_CREAT | O_CLOEXEC | O_RDWR, 0600);
    if (descriptor < 0) {
        return std::unexpected(failure(errc::io, std::strerror(errno), path.string()));
    }
    if (::flock(descriptor, LOCK_EX | LOCK_NB) != 0) {
        const auto lock_error = errno;
        static_cast<void>(::close(descriptor));
        return std::unexpected(failure(lock_error == EWOULDBLOCK ? errc::busy : errc::io,
                                       std::strerror(lock_error),
                                       path.string()));
    }
    return operation_lock{descriptor};
#endif
}

runtime::task<operation_lock> acquire_operation_lock(repository& source) {
    auto root = repository_detail::data_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto acquired = co_await runtime::offload(
        [root = std::move(*root)] { return acquire_operation_lock_blocking(root); });
    if (!acquired) {
        co_return std::unexpected(acquired.error());
    }
    if (!*acquired) {
        co_return std::unexpected(acquired->error());
    }
    co_return std::move(**acquired);
}

auto hex_digit(char value) -> std::optional<unsigned char> {
    if (value >= '0' && value <= '9') {
        return static_cast<unsigned char>(value - '0');
    }
    if (value >= 'a' && value <= 'f') {
        return static_cast<unsigned char>(value - 'a' + 10);
    }
    if (value >= 'A' && value <= 'F') {
        return static_cast<unsigned char>(value - 'A' + 10);
    }
    return std::nullopt;
}

auto percent_decode(std::string_view value) -> result<std::string> {
    auto output = std::string{};
    output.reserve(value.size());
    for (std::size_t index = 0; index < value.size(); ++index) {
        if (value[index] != '%') {
            output.push_back(value[index]);
            continue;
        }
        if (index + 2 >= value.size()) {
            return std::unexpected(failure(errc::invalid_argument, "invalid PostgreSQL URL"));
        }
        const auto high = hex_digit(value[index + 1]);
        const auto low = hex_digit(value[index + 2]);
        if (!high || !low) {
            return std::unexpected(failure(errc::invalid_argument, "invalid PostgreSQL URL"));
        }
        output.push_back(static_cast<char>((*high << 4U) | *low));
        index += 2;
    }
    return output;
}

auto parse_postgres_url(std::string text) -> result<postgres_url> {
    const auto scheme_end = text.find("://");
    if (scheme_end == std::string::npos ||
        (text.substr(0, scheme_end) != "postgresql" && text.substr(0, scheme_end) != "postgres")) {
        return std::unexpected(
            failure(errc::invalid_argument, "data database requires a PostgreSQL URL"));
    }
    const auto path = text.find('/', scheme_end + 3);
    if (path == std::string::npos) {
        return std::unexpected(
            failure(errc::invalid_argument, "PostgreSQL URL must name a database"));
    }
    const auto end = text.find_first_of("?#", path + 1);
    const auto database_end = end == std::string::npos ? text.size() : end;
    const auto encoded = std::string_view{text}.substr(path + 1, database_end - path - 1);
    auto database = percent_decode(encoded);
    if (!database || database->empty() || database->contains('/')) {
        return std::unexpected(
            failure(errc::invalid_argument, "PostgreSQL URL must name one database"));
    }
    return postgres_url{std::move(text), path + 1, database_end, std::move(*database)};
}

auto quote_identifier(std::string_view value) -> std::string {
    auto output = std::string{"\""};
    for (const auto character : value) {
        output += character;
        if (character == '"') {
            output += '"';
        }
    }
    output += '"';
    return output;
}

auto quote_literal(std::string_view value) -> std::string {
    auto output = std::string{"'"};
    for (const auto character : value) {
        output += character;
        if (character == '\'') {
            output += '\'';
        }
    }
    output += '\'';
    return output;
}

auto action_token() -> std::string {
    static auto sequence = std::atomic_uint64_t{};
    auto random = std::random_device{};
    auto output = std::ostringstream{};
    output << std::hex << std::setfill('0') << std::setw(8) << random() << std::setw(8)
           << sequence.fetch_add(1);
    return output.str();
}

auto executable_at(const std::filesystem::path& candidate) -> std::optional<std::filesystem::path> {
    auto code = std::error_code{};
    if (!std::filesystem::is_regular_file(candidate, code) || code) {
        return std::nullopt;
    }
    const auto canonical = std::filesystem::canonical(candidate, code);
    if (code) {
        return std::nullopt;
    }
    return canonical;
}

auto path_program(std::string_view name) -> std::optional<std::filesystem::path> {
    if (const auto* path = std::getenv("PATH")) {
        auto paths = std::stringstream{path};
        for (auto item = std::string{}; std::getline(paths, item, ':');) {
            if (!item.empty()) {
                if (const auto candidate = executable_at(std::filesystem::path{item} / name)) {
                    return candidate;
                }
            }
        }
    }
    return std::nullopt;
}

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

auto pg_config_bindir(const std::filesystem::path& pg_config) -> result<std::filesystem::path> {
#if defined(_WIN32)
    static_cast<void>(pg_config);
    return std::unexpected(
        failure(errc::unsupported, "PostgreSQL data actions are unavailable on Windows"));
#else
    const auto command = shell_quote(pg_config.string()) + " --bindir";
    auto* pipe = ::popen(command.c_str(), "r");
    if (pipe == nullptr) {
        return std::unexpected(failure(errc::io, "cannot run pg_config", pg_config.string()));
    }
    auto output = std::string{};
    auto buffer = std::array<char, 512>{};
    while (std::fgets(buffer.data(), static_cast<int>(buffer.size()), pipe) != nullptr) {
        output += buffer.data();
    }
    const auto status = ::pclose(pipe);
    if (status < 0 || !WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        return std::unexpected(failure(errc::io, "pg_config --bindir failed", pg_config.string()));
    }
    while (!output.empty() && std::isspace(static_cast<unsigned char>(output.back()))) {
        output.pop_back();
    }
    if (output.empty()) {
        return std::unexpected(failure(
            errc::not_found, "pg_config did not report a binary directory", pg_config.string()));
    }
    return std::filesystem::path{output};
#endif
}

auto clients_from_directory(const std::filesystem::path& directory)
    -> std::optional<postgres_clients> {
    const auto psql = executable_at(directory / "psql");
    const auto createdb = executable_at(directory / "createdb");
    const auto dropdb = executable_at(directory / "dropdb");
    const auto pg_dump = executable_at(directory / "pg_dump");
    if (!psql || !createdb || !dropdb || !pg_dump) {
        return std::nullopt;
    }
    return postgres_clients{*psql, *createdb, *dropdb, *pg_dump};
}

auto resolve_postgres_clients_blocking() -> result<postgres_clients> {
    auto pg_config = std::optional<std::filesystem::path>{};
    if (const auto* configured = std::getenv("LIQUID_POSTGRES_BIN")) {
        pg_config = executable_at(std::filesystem::path{configured} / "pg_config");
    }
    if (!pg_config) {
        pg_config = path_program("pg_config");
    }
    if (pg_config) {
        auto bindir = pg_config_bindir(*pg_config);
        if (bindir) {
            if (const auto clients = clients_from_directory(*bindir)) {
                return *clients;
            }
        }
    }
    const auto psql = path_program("psql");
    const auto createdb = path_program("createdb");
    const auto dropdb = path_program("dropdb");
    const auto pg_dump = path_program("pg_dump");
    if (!psql || !createdb || !dropdb || !pg_dump) {
        return std::unexpected(
            failure(errc::not_found, "PostgreSQL client programs are unavailable"));
    }
    return postgres_clients{*psql, *createdb, *dropdb, *pg_dump};
}

runtime::task<postgres_clients> resolve_postgres_clients() {
    auto resolved = co_await runtime::offload(resolve_postgres_clients_blocking);
    if (!resolved) {
        co_return std::unexpected(resolved.error());
    }
    if (!*resolved) {
        co_return std::unexpected(resolved->error());
    }
    co_return std::move(**resolved);
}

auto read_text(const std::filesystem::path& path) -> result<std::string> {
    auto input = std::ifstream{path, std::ios::binary};
    if (!input) {
        return std::unexpected(failure(errc::io, "cannot read data action output", path.string()));
    }
    return std::string{std::istreambuf_iterator<char>{input}, std::istreambuf_iterator<char>{}};
}

auto run_action_blocking(std::filesystem::path action,
                         std::filesystem::path toolchain,
                         data_action_record record) -> result<action_output> {
#if defined(_WIN32)
    static_cast<void>(action);
    static_cast<void>(toolchain);
    static_cast<void>(record);
    return std::unexpected(
        failure(errc::unsupported, "PostgreSQL data actions are unavailable on Windows"));
#else
    const auto work = action / "work";
    const auto sandbox = action / "sandbox";
    const auto output_path = action / "stdout";
    const auto diagnostic_path = action / "stderr";
    auto code = std::error_code{};
    std::filesystem::create_directories(work / "home", code);
    if (!code) {
        std::filesystem::create_directories(work / "tmp", code);
    }
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), action.string()));
    }
    const auto touch = [&](const std::filesystem::path& path) -> result<void> {
        auto stream = std::ofstream{path, std::ios::binary | std::ios::trunc};
        if (!stream) {
            return std::unexpected(failure(errc::io, "cannot prepare data action", path.string()));
        }
        return {};
    };
    const auto hook = action / "hook";
    const auto trace = action / "trace";
    const auto plan = action / "plan";
    if (auto prepared = touch(hook); !prepared) {
        return std::unexpected(prepared.error());
    }
    if (auto prepared = touch(trace); !prepared) {
        return std::unexpected(prepared.error());
    }
    if (auto prepared = touch(plan); !prepared) {
        return std::unexpected(prepared.error());
    }
    auto command = std::vector<std::string>{record.executable.string()};
    command.insert(command.end(), record.arguments.begin(), record.arguments.end());
    record.environment.insert_or_assign("PATH", record.executable.parent_path().string());
    record.environment.insert_or_assign("HOME", (work / "home").string());
    record.environment.insert_or_assign("TMPDIR", (work / "tmp").string());
    record.environment.insert_or_assign("LANG", "C.UTF-8");
    record.environment.insert_or_assign("TZ", "UTC");
    record.environment.insert_or_assign("SOURCE_DATE_EPOCH", "0");
    auto launched = sandbox_detail::launch_wrap({.sandbox_root = sandbox,
                                                 .input_mount = work,
                                                 .toolchain_mount = toolchain,
                                                 .hook = hook,
                                                 .trace = trace,
                                                 .plan = plan,
                                                 .stdout_path = output_path,
                                                 .stderr_path = diagnostic_path,
                                                 .command = std::move(command),
                                                 .environment = std::move(record.environment),
                                                 .network = true});
    if (!launched) {
        return std::unexpected(launched.error());
    }
    auto output = read_text(output_path);
    auto diagnostic = read_text(diagnostic_path);
    if (!output || !diagnostic) {
        return std::unexpected(output ? diagnostic.error() : output.error());
    }
    return action_output{launched->exit, std::move(*output), std::move(*diagnostic)};
#endif
}

runtime::task<std::string> run_client(repository& source,
                                      std::filesystem::path executable,
                                      std::vector<std::string> arguments,
                                      std::map<std::string, std::string> environment = {}) {
    auto root = repository_detail::data_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    const auto action = *root / ".liquid-data" / "actions" / action_token();
    const auto toolchain = action / "toolchain";
    auto prepared = co_await runtime::offload([toolchain] {
        auto code = std::error_code{};
        std::filesystem::create_directories(toolchain, code);
        if (code) {
            return result<void>{
                std::unexpected(failure(errc::io, code.message(), toolchain.string()))};
        }
        return result<void>{};
    });
    if (!prepared) {
        co_return std::unexpected(prepared.error());
    }
    if (!*prepared) {
        static_cast<void>(co_await runtime::offload([action] {
            auto code = std::error_code{};
            std::filesystem::remove_all(action, code);
        }));
        co_return std::unexpected(prepared->error());
    }
    auto ran = co_await runtime::offload(
        [action,
         toolchain,
         record = data_action_record{
             std::move(executable), std::move(arguments), std::move(environment)}]() mutable {
            auto output = run_action_blocking(action, toolchain, std::move(record));
            auto code = std::error_code{};
            std::filesystem::remove_all(action, code);
            return output;
        });
    if (!ran) {
        co_return std::unexpected(ran.error());
    }
    if (!*ran) {
        co_return std::unexpected(ran->error());
    }
    if ((*ran)->exit != 0) {
        auto diagnostic = std::move((*ran)->diagnostic);
        if (diagnostic.empty()) {
            diagnostic = std::move((*ran)->output);
        }
        while (!diagnostic.empty() && std::isspace(static_cast<unsigned char>(diagnostic.back()))) {
            diagnostic.pop_back();
        }
        if (diagnostic.empty()) {
            diagnostic =
                "PostgreSQL client action exited with status " + std::to_string((*ran)->exit);
        }
        co_return std::unexpected(failure(errc::io, std::move(diagnostic), executable.string()));
    }
    co_return std::move((*ran)->output);
}

runtime::task<void> run_psql(repository& source,
                             const postgres_clients& clients,
                             const postgres_url& base,
                             std::string statement) {
    auto output = co_await run_client(source,
                                      clients.psql,
                                      {"--no-psqlrc",
                                       "--set",
                                       "ON_ERROR_STOP=1",
                                       "--dbname",
                                       base.with_database("postgres"),
                                       "--command",
                                       std::move(statement)});
    if (!output) {
        co_return std::unexpected(output.error());
    }
    co_return result<void>{};
}

runtime::task<void> create_database(repository& source,
                                    const postgres_clients& clients,
                                    const postgres_url& base,
                                    std::string database,
                                    std::string template_database) {
    auto output = co_await run_client(source,
                                      clients.createdb,
                                      {"--maintenance-db",
                                       base.with_database("postgres"),
                                       "--template",
                                       std::move(template_database),
                                       std::move(database)});
    if (!output) {
        co_return std::unexpected(output.error());
    }
    co_return result<void>{};
}

runtime::task<void> drop_database(repository& source,
                                  const postgres_clients& clients,
                                  const postgres_url& base,
                                  std::string database) {
    auto output = co_await run_client(source,
                                      clients.dropdb,
                                      {"--maintenance-db",
                                       base.with_database("postgres"),
                                       "--force",
                                       "--if-exists",
                                       std::move(database)});
    if (!output) {
        co_return std::unexpected(output.error());
    }
    co_return result<void>{};
}

runtime::task<std::string>
fingerprint(repository& source, const postgres_clients& clients, std::string url) {
    auto dump = co_await run_client(source,
                                    clients.pg_dump,
                                    {"--dbname",
                                     std::move(url),
                                     "--no-owner",
                                     "--no-privileges",
                                     "--inserts",
                                     "--column-inserts"});
    if (!dump) {
        co_return std::unexpected(dump.error());
    }
    auto normalized = std::ostringstream{};
    auto lines = std::istringstream{*dump};
    for (auto line = std::string{}; std::getline(lines, line);) {
        if (line.starts_with("\\restrict ") || line.starts_with("\\unrestrict ")) {
            continue;
        }
        normalized << line << '\n';
    }
    const auto text = std::move(normalized).str();
    const auto bytes = std::span{reinterpret_cast<const std::byte*>(text.data()), text.size()};
    auto value = hash(algorithm::blake3, bytes);
    if (!value) {
        co_return std::unexpected(value.error());
    }
    co_return to_string(*value);
}

auto database_token(std::string_view value) -> std::string {
    auto output = std::string{};
    for (const auto character : value) {
        output += std::isalnum(static_cast<unsigned char>(character))
                      ? static_cast<char>(std::tolower(static_cast<unsigned char>(character)))
                      : '_';
    }
    return output;
}

auto branch_name(const data_database& database, const session_id& session, std::string_view token)
    -> std::string {
    auto output = "liquid_" + database_token(database.name) + "_" +
                  to_string(session).substr(0, 12) + "_" + std::string{token.substr(0, 8)};
    if (output.size() > 63) {
        output.resize(63);
    }
    return output;
}

auto find_stored_branch(repository& source, std::string_view id)
    -> result<repository_detail::stored_data_branch> {
    auto branches = repository_detail::data_access::branches(source);
    if (!branches) {
        return std::unexpected(branches.error());
    }
    const auto found = std::ranges::find(
        *branches, id, [](const auto& branch) { return std::string_view{branch.record.id}; });
    if (found == branches->end()) {
        return std::unexpected(
            failure(errc::not_found, "data branch is not recorded", std::string{id}));
    }
    return *found;
}

auto reflink_probe(std::filesystem::path destination) -> result<bool> {
    auto code = std::error_code{};
    std::filesystem::create_directories(destination, code);
    if (code) {
        return std::unexpected(failure(errc::io, code.message(), destination.string()));
    }
    const auto token = action_token();
    const auto source = destination / ("reflink-source-" + token);
    const auto clone = destination / ("reflink-clone-" + token);
    {
        auto output = std::ofstream{source, std::ios::binary | std::ios::trunc};
        output << "liquid reflink probe\n";
        if (!output) {
            return std::unexpected(
                failure(errc::io, "cannot write reflink probe", source.string()));
        }
    }
    auto supported = false;
#if defined(__APPLE__)
    supported = ::clonefile(source.c_str(), clone.c_str(), 0) == 0;
#elif defined(__linux__)
    const auto input = ::open(source.c_str(), O_RDONLY);
    const auto output = ::open(clone.c_str(), O_CREAT | O_TRUNC | O_WRONLY, 0600);
    supported = input >= 0 && output >= 0 && ::ioctl(output, FICLONE, input) == 0;
    if (input >= 0) {
        ::close(input);
    }
    if (output >= 0) {
        ::close(output);
    }
#endif
    std::filesystem::remove(source, code);
    code.clear();
    std::filesystem::remove(clone, code);
    return supported;
}

runtime::task<void> restore_connections(repository& source,
                                        const postgres_clients& clients,
                                        const postgres_url& base,
                                        std::string database) {
    co_return co_await run_psql(source,
                                clients,
                                base,
                                "ALTER DATABASE " + quote_identifier(database) +
                                    " WITH ALLOW_CONNECTIONS true");
}

} // namespace

std::string_view to_string(data_branch_mechanism mechanism) noexcept {
    switch (mechanism) {
    case data_branch_mechanism::template_copy:
        return "template";
    case data_branch_mechanism::reflink:
        return "reflink";
    case data_branch_mechanism::automatic:
        return "auto";
    }
    return "auto";
}

namespace data_detail {

result<std::map<std::string, std::string>> session_environment(repository& source,
                                                               session_id session) {
    auto configuration = repository_detail::data_access::configuration(source);
    auto branches = repository_detail::data_access::branches(source);
    if (!configuration || !branches) {
        return std::unexpected(configuration ? branches.error() : configuration.error());
    }
    auto output = std::map<std::string, std::string>{};
    for (const auto& database : configuration->databases) {
        const auto found = std::ranges::find_if(*branches, [&](const auto& branch) {
            return branch.record.session == session && branch.record.database == database.name;
        });
        if (found == branches->end()) {
            return std::unexpected(
                failure(errc::not_found, "session has no live data branch", database.name));
        }
        output.insert_or_assign(database.url_env, found->record.url);
    }
    return output;
}

namespace testing {

runtime::task<data_branch_mechanism> select_mechanism(data_branch_mechanism requested,
                                                      std::filesystem::path destination,
                                                      bool local_quiescent) {
    if (requested == data_branch_mechanism::template_copy) {
        co_return data_branch_mechanism::template_copy;
    }
    if (!local_quiescent) {
        if (requested == data_branch_mechanism::reflink) {
            co_return std::unexpected(failure(
                errc::unsupported, "reflink requires a local quiescent PostgreSQL cluster"));
        }
        co_return data_branch_mechanism::template_copy;
    }
    auto supported = co_await runtime::offload(
        [destination = std::move(destination)] { return reflink_probe(destination); });
    if (!supported) {
        co_return std::unexpected(supported.error());
    }
    if (!*supported) {
        if (requested == data_branch_mechanism::reflink) {
            co_return std::unexpected(
                failure(errc::unsupported, "destination filesystem does not support reflink"));
        }
        co_return data_branch_mechanism::template_copy;
    }
    co_return data_branch_mechanism::reflink;
}

runtime::task<std::string>
current_database(repository& source, session_id session, std::string url_environment) {
    auto environment = session_environment(source, session);
    if (!environment) {
        co_return std::unexpected(environment.error());
    }
    const auto found = environment->find(url_environment);
    if (found == environment->end()) {
        co_return std::unexpected(failure(errc::not_found,
                                          "session data URL environment is not configured",
                                          std::move(url_environment)));
    }
    auto clients = co_await resolve_postgres_clients();
    if (!clients) {
        co_return std::unexpected(clients.error());
    }
    auto output = co_await run_client(source,
                                      clients->psql,
                                      {"--no-psqlrc",
                                       "--tuples-only",
                                       "--no-align",
                                       "--dbname",
                                       found->second,
                                       "--command",
                                       "SELECT current_database()"});
    if (!output) {
        co_return std::unexpected(output.error());
    }
    while (!output->empty() && std::isspace(static_cast<unsigned char>(output->back()))) {
        output->pop_back();
    }
    co_return std::move(*output);
}

} // namespace testing
} // namespace data_detail

namespace data_branch {

runtime::task<data_branch_record>
create(repository& source, session_id session, data_database database) {
    auto guard = co_await acquire_operation_lock(source);
    if (!guard) {
        co_return std::unexpected(guard.error());
    }
    auto operation = std::move(*guard);
    auto configuration = repository_detail::data_access::configuration(source);
    if (!configuration) {
        co_return std::unexpected(configuration.error());
    }
    const auto configured =
        std::ranges::find(configuration->databases, database.name, &data_database::name);
    if (configured == configuration->databases.end()) {
        co_return std::unexpected(
            failure(errc::not_found, "data database is not configured", database.name));
    }
    database = *configured;
    auto branches = repository_detail::data_access::branches(source);
    if (!branches) {
        co_return std::unexpected(branches.error());
    }
    if (std::ranges::any_of(*branches, [&](const auto& branch) {
            return branch.record.session == session && branch.record.database == database.name;
        })) {
        co_return std::unexpected(failure(
            errc::already_exists, "data branch already exists for the session", database.name));
    }
    auto root = repository_detail::data_access::root_path(source);
    if (!root) {
        co_return std::unexpected(root.error());
    }
    auto mechanism = co_await data_detail::testing::select_mechanism(
        database.mechanism, *root / ".liquid-data" / "reflink-probe", false);
    if (!mechanism) {
        co_return std::unexpected(mechanism.error());
    }
    if (*mechanism == data_branch_mechanism::reflink) {
        co_return std::unexpected(
            failure(errc::unsupported, "reflink branch creation requires local cluster metadata"));
    }
    auto base = parse_postgres_url(database.base_url);
    if (!base) {
        co_return std::unexpected(base.error());
    }
    auto clients = co_await resolve_postgres_clients();
    if (!clients) {
        co_return std::unexpected(clients.error());
    }
    const auto id = action_token();
    const auto name = branch_name(database, session, id);
    auto created = co_await create_database(source, *clients, *base, name, base->database);
    if (!created) {
        co_return std::unexpected(created.error());
    }
    const auto url = base->with_database(name);
    auto base_fingerprint = co_await fingerprint(source, *clients, url);
    if (!base_fingerprint) {
        static_cast<void>(co_await drop_database(source, *clients, *base, name));
        co_return std::unexpected(base_fingerprint.error());
    }
    auto record = data_branch_record{
        .id = id,
        .session = session,
        .database = database.name,
        .url = url,
        .mechanism = *mechanism,
        .created_at = std::chrono::floor<std::chrono::seconds>(std::chrono::system_clock::now())};
    auto saved = co_await repository_detail::data_access::add(
        source,
        {.record = record,
         .base_url = database.base_url,
         .base_fingerprint = std::move(*base_fingerprint)});
    if (!saved) {
        static_cast<void>(co_await drop_database(source, *clients, *base, name));
        co_return std::unexpected(saved.error());
    }
    co_return record;
}

runtime::task<std::vector<data_branch_record>> list(repository& source) {
    auto branches = repository_detail::data_access::branches(source);
    if (!branches) {
        co_return std::unexpected(branches.error());
    }
    auto output = std::vector<data_branch_record>{};
    output.reserve(branches->size());
    for (const auto& branch : *branches) {
        output.push_back(branch.record);
    }
    std::ranges::stable_sort(output, {}, &data_branch_record::created_at);
    co_return output;
}

runtime::task<void> discard(repository& source, std::string id) {
    auto guard = co_await acquire_operation_lock(source);
    if (!guard) {
        co_return std::unexpected(guard.error());
    }
    auto operation = std::move(*guard);
    auto branch = find_stored_branch(source, id);
    if (!branch) {
        co_return std::unexpected(branch.error());
    }
    auto base = parse_postgres_url(branch->base_url);
    if (!base) {
        co_return std::unexpected(base.error());
    }
    auto branch_url = parse_postgres_url(branch->record.url);
    if (!branch_url) {
        co_return std::unexpected(branch_url.error());
    }
    auto clients = co_await resolve_postgres_clients();
    if (!clients) {
        co_return std::unexpected(clients.error());
    }
    auto dropped = co_await drop_database(source, *clients, *base, branch_url->database);
    if (!dropped) {
        co_return std::unexpected(dropped.error());
    }
    co_return co_await repository_detail::data_access::remove(source, std::move(id));
}

runtime::task<void> promote(repository& source, std::string id) {
    auto guard = co_await acquire_operation_lock(source);
    if (!guard) {
        co_return std::unexpected(guard.error());
    }
    auto operation = std::move(*guard);
    auto branch = find_stored_branch(source, id);
    if (!branch) {
        co_return std::unexpected(branch.error());
    }
    auto base = parse_postgres_url(branch->base_url);
    auto branch_url = parse_postgres_url(branch->record.url);
    if (!base || !branch_url) {
        co_return std::unexpected(base ? branch_url.error() : base.error());
    }
    auto clients = co_await resolve_postgres_clients();
    if (!clients) {
        co_return std::unexpected(clients.error());
    }
    const auto token = action_token();
    const auto comparison = branch_name({.name = "compare"}, branch->record.session, token);
    const auto backup = branch_name({.name = "backup"}, branch->record.session, token);
    auto locked = co_await run_psql(source,
                                    *clients,
                                    *base,
                                    "ALTER DATABASE " + quote_identifier(base->database) +
                                        " WITH ALLOW_CONNECTIONS false");
    if (!locked) {
        co_return std::unexpected(locked.error());
    }
    auto terminated = co_await run_psql(
        source,
        *clients,
        *base,
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = " +
            quote_literal(base->database) + " AND pid <> pg_backend_pid()");
    if (!terminated) {
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        co_return std::unexpected(terminated.error());
    }
    auto copied = co_await create_database(source, *clients, *base, comparison, base->database);
    if (!copied) {
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        co_return std::unexpected(copied.error());
    }
    auto current_fingerprint =
        co_await fingerprint(source, *clients, base->with_database(comparison));
    if (!current_fingerprint || *current_fingerprint != branch->base_fingerprint) {
        static_cast<void>(co_await drop_database(source, *clients, *base, comparison));
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        if (!current_fingerprint) {
            co_return std::unexpected(current_fingerprint.error());
        }
        co_return std::unexpected(failure(
            errc::conflict, "base database changed after the branch point", base->database));
    }
    auto branch_locked =
        co_await run_psql(source,
                          *clients,
                          *base,
                          "ALTER DATABASE " + quote_identifier(branch_url->database) +
                              " WITH ALLOW_CONNECTIONS false");
    if (!branch_locked) {
        static_cast<void>(co_await drop_database(source, *clients, *base, comparison));
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        co_return std::unexpected(branch_locked.error());
    }
    auto branch_terminated = co_await run_psql(
        source,
        *clients,
        *base,
        "SELECT pg_terminate_backend(pid) FROM pg_stat_activity WHERE datname = " +
            quote_literal(branch_url->database) + " AND pid <> pg_backend_pid()");
    if (!branch_terminated) {
        static_cast<void>(
            co_await restore_connections(source, *clients, *base, branch_url->database));
        static_cast<void>(co_await drop_database(source, *clients, *base, comparison));
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        co_return std::unexpected(branch_terminated.error());
    }
    auto renamed_base = co_await run_psql(source,
                                          *clients,
                                          *base,
                                          "ALTER DATABASE " + quote_identifier(base->database) +
                                              " RENAME TO " + quote_identifier(backup));
    if (!renamed_base) {
        static_cast<void>(
            co_await restore_connections(source, *clients, *base, branch_url->database));
        static_cast<void>(co_await drop_database(source, *clients, *base, comparison));
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        co_return std::unexpected(renamed_base.error());
    }
    auto renamed_branch =
        co_await run_psql(source,
                          *clients,
                          *base,
                          "ALTER DATABASE " + quote_identifier(branch_url->database) +
                              " RENAME TO " + quote_identifier(base->database));
    if (!renamed_branch) {
        static_cast<void>(co_await run_psql(source,
                                            *clients,
                                            *base,
                                            "ALTER DATABASE " + quote_identifier(backup) +
                                                " RENAME TO " + quote_identifier(base->database)));
        static_cast<void>(
            co_await restore_connections(source, *clients, *base, branch_url->database));
        static_cast<void>(co_await restore_connections(source, *clients, *base, base->database));
        static_cast<void>(co_await drop_database(source, *clients, *base, comparison));
        co_return std::unexpected(renamed_branch.error());
    }
    auto enabled = co_await restore_connections(source, *clients, *base, base->database);
    if (!enabled) {
        co_return std::unexpected(enabled.error());
    }
    auto old_base = co_await drop_database(source, *clients, *base, backup);
    auto comparison_dropped = co_await drop_database(source, *clients, *base, comparison);
    if (!old_base || !comparison_dropped) {
        co_return std::unexpected(old_base ? comparison_dropped.error() : old_base.error());
    }
    co_return co_await repository_detail::data_access::remove(source, std::move(id));
}

} // namespace data_branch
} // namespace liquid
