#pragma once

// Named lakes: which lake liquid works with, the token for each, the lake a repository syncs with,
// and the version check made before the first sync with a lake. See docs/library/lakes.md.

#include <cstdint>
#include <filesystem>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "liquid/error.hpp"
#include "liquid/export.hpp"
#include "liquid/runtime.hpp"

namespace liquid {

class repository;

// One lake the user has named. `url` is the lake's base URL: scheme, host, optional port and an
// optional path prefix, with no trailing slash. Requests go to `<url>/v1/...`.
struct LIQUID_EXPORT lake {
    std::string name;
    std::string url;

    auto operator==(const lake&) const -> bool = default;
};

// Where a lake's token lives. Tokens are keyed by the lake's origin (scheme, host and port), so a
// token stored for one lake can never be read for another, even when a name is reused.
class LIQUID_EXPORT lake_credential_store {
public:
    virtual ~lake_credential_store();
    [[nodiscard]] virtual auto read(std::string_view origin)
        -> result<std::optional<std::string>> = 0;
    [[nodiscard]] virtual auto write(std::string_view origin, std::string_view token)
        -> result<void> = 0;
    [[nodiscard]] virtual auto erase(std::string_view origin) -> result<void> = 0;
};

// The operating system's credential store: the login keychain on macOS, Credential Manager on
// Windows. Elsewhere it holds nothing and refuses writes with `unavailable`; tokens then come from
// the environment. `LIQUID_CREDENTIAL_STORE=none` turns it off on every platform.
[[nodiscard]] LIQUID_EXPORT auto os_lake_credential_store()
    -> std::shared_ptr<lake_credential_store>;
// A store that lives only in this process, for tests and for callers that keep tokens themselves.
[[nodiscard]] LIQUID_EXPORT auto memory_lake_credential_store()
    -> std::shared_ptr<lake_credential_store>;

struct LIQUID_EXPORT lake_registry_options {
    // The list of named lakes. Empty means `$LIQUID_HOME/lakes.json`, or `~/.liquid/lakes.json`.
    std::filesystem::path path;
    // Null means os_lake_credential_store().
    std::shared_ptr<lake_credential_store> credentials;
};

// Where a token came from. Never the token itself.
enum class lake_token_source : std::uint8_t {
    none = 0,
    credential_store = 1,
    lake_environment = 2,   // LIQUID_TOKEN_<NAME>
    shared_environment = 3, // LIQUID_TOKEN
};

struct LIQUID_EXPORT lake_token {
    std::string value;
    lake_token_source source{lake_token_source::none};
};

// The user's named lakes. There is no built-in lake: an empty list is the starting state.
class LIQUID_EXPORT lake_registry {
public:
    lake_registry();
    ~lake_registry();
    lake_registry(lake_registry&&) noexcept;
    lake_registry& operator=(lake_registry&&) noexcept;
    lake_registry(const lake_registry&) = delete;
    lake_registry& operator=(const lake_registry&) = delete;

    [[nodiscard]] static runtime::task<lake_registry> open(lake_registry_options options = {});

    [[nodiscard]] auto list() const -> std::span<const lake>;
    [[nodiscard]] auto find(std::string_view name) const -> std::optional<lake>;
    // The named lake a full URL belongs to: same origin, and the lake's path prefix (if any) is a
    // prefix of the URL's path. The longest prefix wins.
    [[nodiscard]] auto match(std::string_view url) const -> std::optional<lake>;

    // Names are 1 to 64 characters of a-z, 0-9, '-' and '_', starting with a letter or digit.
    [[nodiscard]] runtime::task<lake> add(std::string name, std::string url);
    // Removes the lake and erases its stored token.
    [[nodiscard]] runtime::task<void> remove(std::string name);

    [[nodiscard]] runtime::task<void> set_token(std::string name, std::string token);
    [[nodiscard]] runtime::task<void> clear_token(std::string name);
    // The token for a named lake: the credential store, then LIQUID_TOKEN_<NAME> (upper case, '-'
    // as '_'), then LIQUID_TOKEN. Empty with source `none` when there is none.
    [[nodiscard]] runtime::task<lake_token> token(std::string name) const;

private:
    struct impl;
    std::unique_ptr<impl> impl_;
};

// The lake a repository syncs with: the lake's name and the repository's path under the lake's
// URL (for example `/v1/repositories/<id>`). Kept in the repository's local metadata, never synced.
struct LIQUID_EXPORT repository_lake {
    std::string lake;
    std::string repository;

    auto operator==(const repository_lake&) const -> bool = default;
};

[[nodiscard]] LIQUID_EXPORT runtime::task<std::optional<repository_lake>>
read_repository_lake(repository& subject);
[[nodiscard]] LIQUID_EXPORT runtime::task<void> write_repository_lake(repository& subject,
                                                                      repository_lake value);

// `<id>`, `/path`, or a full URL of `target`, as a repository path under `target`'s URL.
[[nodiscard]] LIQUID_EXPORT auto repository_path_for(const lake& target,
                                                     std::string_view repository)
    -> result<std::string>;

// What one sync, review or clone talks to.
struct LIQUID_EXPORT lake_target {
    std::optional<lake> named; // the named lake, when the URL belongs to one
    std::string base_url;      // the lake's base URL; server_info is `<base_url>/v1/server_info`
    std::string repository_url;
    lake_token token; // only this lake's token
};

// Resolves what a command talks to. `argument` is empty (the repository's recorded lake), a lake
// name (that lake, with the repository's recorded path), or a full URL (matched to a named lake by
// host; an unnamed URL gets only LIQUID_TOKEN). `subject` may be null outside a repository.
[[nodiscard]] LIQUID_EXPORT runtime::task<lake_target>
resolve_lake_target(const lake_registry& lakes, repository* subject, std::string argument);

// What a lake says about itself in `GET /v1/server_info`. Empty fields were not reported.
struct LIQUID_EXPORT lake_server_info {
    std::string api;
    std::string name;
    std::string version;
    std::string revision;
    std::vector<std::uint32_t> sync_protocol_versions;
    std::vector<std::uint32_t> forge_api_versions;
    std::string liquid_oldest;
    std::string liquid_newest;
};

// The sync protocol and forge API versions this liquid speaks.
[[nodiscard]] LIQUID_EXPORT auto supported_sync_protocol_versions()
    -> std::span<const std::uint32_t>;
[[nodiscard]] LIQUID_EXPORT auto supported_forge_api_versions() -> std::span<const std::uint32_t>;

enum class lake_compatibility_kind : std::uint8_t {
    compatible = 1,
    unknown = 2,     // the lake did not report versions, or could not be asked; allowed
    unsupported = 3, // refused
};

struct LIQUID_EXPORT lake_compatibility {
    lake_compatibility_kind kind{lake_compatibility_kind::unknown};
    std::string message; // one line naming the lake, its version and the range liquid supports
    lake_server_info info;
};

// Reads `<base_url>/v1/server_info`. No token is sent.
[[nodiscard]] LIQUID_EXPORT runtime::task<lake_server_info>
fetch_lake_server_info(std::string base_url);

// Decides from what the lake reported. `label` names the lake in the message.
[[nodiscard]] LIQUID_EXPORT auto check_lake_compatibility(std::string_view label,
                                                          const lake_server_info& info)
    -> lake_compatibility;

// Fetches and decides once per base URL for the life of the process: the check made before the
// first sync of a session. A lake that cannot be asked is `unknown`, with the reason.
[[nodiscard]] LIQUID_EXPORT runtime::task<lake_compatibility> check_lake(const lake_target& target);

// The same check, returning an `unsupported` error for a refused lake.
[[nodiscard]] LIQUID_EXPORT runtime::task<lake_compatibility>
require_compatible_lake(const lake_target& target);

} // namespace liquid
