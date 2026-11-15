# Named lakes

liquid works with any number of lakes: one you run, a colleague's, a hosted one, a test instance.
A lake is configuration, not a string typed into every command. You name each lake once, each
repository records which lake it syncs with, each lake has its own token, and liquid checks a
lake's version before it syncs with it. There is no built-in lake: a fresh install knows none
(liquid#525).

The public API is [`include/liquid/lake.hpp`](../../include/liquid/lake.hpp), in libliquid, so the
`liquid` command and an IDE that links the library make the same choices. The commands are
[`liquid lake`](../reference/cli/lake.md), and `push`, `pull`, `clone` and `review` use them.

## The list of lakes

`lake_registry::open()` reads `$LIQUID_HOME/lakes.json` (`~/.liquid/lakes.json` when `LIQUID_HOME`
is unset). The file holds names and URLs only, never a token:

```json
{
  "v": 1,
  "lakes": [
    { "name": "dev", "url": "https://lake.example.invalid" }
  ]
}
```

- A name is 1 to 64 characters of `a-z`, `0-9`, `-` and `_`, starting with a letter or digit.
- A URL is `http` or `https`, a host, an optional port and an optional path prefix. A trailing
  slash is dropped; a query or fragment is refused. Requests go to `<url>/v1/...`.
- Two names for the same URL (same origin, same prefix) are refused, so a URL always matches one
  lake.
- `match(url)` finds the lake a full URL belongs to: the same origin (scheme, host and port, with
  default ports made explicit) and the lake's path prefix at a segment boundary. The longest prefix
  wins.

| C++ | CLI |
|---|---|
| `lake_registry::add(name, url)` | `liquid lake add <name> <url>` |
| `lake_registry::list()`, `find(name)`, `token(name)` | `liquid lake list [--json]` |
| `lake_registry::remove(name)` (also erases its stored token) | `liquid lake remove <name>` |
| `lake_registry::set_token(name, token)` | `liquid lake login <name>` (token on standard input) |
| `lake_registry::clear_token(name)` | `liquid lake logout <name>` |
| `write_repository_lake(repository, {lake, repository_path})` | `liquid lake use <name> [<repository>]` |
| `check_lake(target)` | `liquid lake check [<name>] [--json]` |

## The lake a repository syncs with

`write_repository_lake` stores `{v: 1, lake, repository}` as canonical CBOR under the repository's
local metadata key `lake.v1`. It is never synced: which lake a checkout talks to is the checkout's
choice. `repository` is the path under the lake's URL, normally `/v1/repositories/<id>`.
`repository_path_for(lake, value)` turns an identifier, a path, or a full URL on that lake into
that path.

- **Clone** records the lake when the URL belongs to a named lake, or when the lake is named:
  `liquid clone --lake dev <id> [<dir>]`.
- **Push and pull** record it the first time a repository with no recorded lake syncs with a URL
  on a named lake.
- **`liquid lake use <name> [<repository>]`** chooses or changes it. Without `<repository>` the
  recorded path is kept, so moving a repository to another lake where it has the same identifier is
  `liquid lake use <other>`.

`resolve_lake_target(lakes, repository, argument)` decides what one command talks to:

| `argument` | Lake | Repository URL | Token |
|---|---|---|---|
| empty | the repository's recorded lake | lake URL + recorded path | that lake's |
| a lake name | that lake | lake URL + recorded path | that lake's |
| a full URL on a named lake | that lake | the URL | that lake's |
| a full URL on no named lake | none | the URL | `LIQUID_TOKEN` only |

With nothing recorded and no lakes named, the error says to run `liquid lake add <name> <url>` and
`liquid lake use <name> <repository>`, or to give a full URL.

## Tokens

A token is looked up for one lake, in this order:

1. The operating system's credential store, keyed by the lake's origin and path prefix: a generic
   password with service `liquid lake` in the login keychain on macOS, a generic credential named
   `liquid-lake:<key>` in Credential Manager on Windows.
2. `LIQUID_TOKEN_<NAME>`, the lake's name in upper case with `-` as `_` (`dev-eu` reads
   `LIQUID_TOKEN_DEV_EU`).
3. `LIQUID_TOKEN`, the fallback for scripts. It goes to whichever lake the command talks to, so a
   script that uses more than one lake sets the per-lake variables instead.

A token is never written to the lake list, to repository metadata or to any other file liquid
writes, and `lake list` shows only where it comes from (`credential_store`, `lake_environment`,
`shared_environment` or `none`). Because the store is keyed by the lake's origin and prefix, a
stored token cannot be read for a different lake, even after a name is removed and reused.

`LIQUID_CREDENTIAL_STORE=none` turns the operating-system store off. On Linux and other platforms
liquid has no store it writes to yet: `lake login` refuses with `unavailable` and names the
`LIQUID_TOKEN_<NAME>` variable to use. A Secret Service client of liquid's own is not written.

Callers that keep tokens themselves pass a `lake_credential_store` in `lake_registry_options`.
`memory_lake_credential_store()` holds them for the life of the process; the tests use it.

## The version check

Before the first sync, clone or review with a lake in a process, liquid reads
`GET <lake url>/v1/server_info` without a token and decides whether it can work with that lake.
The answer is kept for the life of the process, so a long-running caller checks each lake once.
Every request liquid makes also carries `Liquid-Version: <liquid version>`, so a lake can refuse
an unsupported client with a clear error of its own.

liquid reads the `liquid` object of `server_info`, inside lake's `{"ok": true, "value": ...}`
envelope or as a bare object. The contract is lake's `docs/api/liquid-clients.md`
(oramasearch/lake#1324, served since oramasearch/lake#1330):

```json
{"ok": true, "value": {"api": "v1", "name": "lake", "revision": "2e6024cf...",
 "liquid": {"sync_protocol": 1, "sync_media_type": "application/vnd.liquid.sync-v1",
            "forge_api": 1, "oldest_release": "0.1.0", "newest_release": "0.2.0",
            "version_header": "Liquid-Version"}}, "duration_ms": 0}
```

| Field | Read as |
|---|---|
| `revision` (top level) | The lake's build, shortened to 12 characters in messages |
| `liquid.sync_protocol` | The sync protocol versions the lake serves: an integer, or an array when a lake serves several (`application/vnd.liquid.sync-v1` is `1`) |
| `liquid.forge_api` | The review (forge) API versions, integer or array |
| `liquid.oldest_release` | The oldest liquid release the lake supports |
| `liquid.newest_release` | The newest liquid release the lake supports (the libliquid it links) |

A top-level `version` string is shown when a lake serves one; lake#1330 does not. A lake with no
`liquid` object is `unknown`.

This liquid speaks sync protocol `1` and forge API `1` (`supported_sync_protocol_versions()`,
`supported_forge_api_versions()`).

`check_lake_compatibility(label, info)` decides:

| Lake reports | Result |
|---|---|
| no `liquid` object (or none of its fields) | `unknown`: allowed, with a warning |
| no sync protocol this liquid speaks | `unsupported`: upgrade the side whose newest protocol is older |
| `oldest_release` newer than this liquid | `unsupported`: upgrade liquid |
| `newest_release` older than this liquid | `unsupported`: upgrade the lake, or use a liquid it supports |
| otherwise | `compatible` |

A lake that does not answer `server_info` (an older lake, a plain sync server, a network error) is
`unknown`, and the command continues with a warning. An unsupported lake is refused before any
request carries a token, with one message naming the lake, its version and the range this liquid
supports:

```text
error: unsupported: lake 'future' (revision abc, sync protocol 9) does not work with this
liquid (liquid 0.2.0 supports sync protocol 1): upgrade liquid (future)
```

The lake refuses on its side too. A request whose `Liquid-Version` is outside its range gets HTTP
400 `unsupported_client_version`. The forge client maps that REST error, and the sync client maps
the sync error body (code `unsupported`, context `unsupported_client_version`), to the same
`errc::unsupported` error carrying lake's message, so both print as
`error: unsupported: <lake's message> (unsupported_client_version)`.

`require_compatible_lake(target)` is `check_lake` with an `unsupported` result turned into an
`errc::unsupported` error; the CLI uses it, and prints the `unknown` message as a warning.

## How to verify

`tests/test_lake.cpp` runs against two in-process lake doubles
([`forge_double`](forge-client.md), which answers `GET /v1/server_info` with a body the test
sets and records the bearer on sync requests):

- the list starts empty, persists, refuses bad names and duplicate URLs, and matches URLs by host
  and prefix;
- two lakes with different tokens each receive only their own, and a repository moves from one to
  the other with `write_repository_lake` and syncs with the second lake's token;
- per-lake variables come before `LIQUID_TOKEN`, a stored token before both, and an unnamed URL
  gets only `LIQUID_TOKEN`;
- the exact `server_info` lake#1330 serves is read field by field, a bare object and an array of
  protocols are read too, and a lake with no `liquid` object is `unknown`;
- a lake reporting unsupported versions is refused with the message above, and asked once;
- lake's `unsupported_client_version` refusal reads the same from a REST call and a sync path;
- a lake that reports nothing is allowed with a warning;
- with the built binary: `lake add`, `clone --lake`, `pull` with no URL, `lake use` to move,
  `lake check`, the error with no lake configured, and a refused push that sent no token.

## Limits

- No operating-system credential store on Linux yet; tokens come from the environment there.
- The build cache's remote endpoints are configured separately and do not use named lakes yet.
- The check reads `server_info` once per process; a lake upgraded while a process runs is checked
  again only by a new process.
