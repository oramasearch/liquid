# The `.liquid` configuration language

**What.** Issue #74 adds liquid's small typed configuration language, static checker, lowering,
canonical formatter, source fetch, action-backed generator, and configured target command. The
language is data: it has no functions, lambdas, evaluation order, laziness, or network imports.

**Why.** Package rules and build actions need a reviewable source form without inheriting the
evaluation model and distant failures of a general-purpose build language. An ordinary sandboxed
program covers cases the declarative form cannot express while retaining action identity,
provenance, caching, and traceability.

**Implementation status.** The public parser, checker, lowerer, formatter, `package/1`, `target/1`,
`source/1`, and `install_intent/1` projection, lock-rule lowering, repository-local includes,
digest-verifying source fetch, generators, relocation-checked target execution, and CLI commands
are implemented. Install layout and validated declarative intent are retained on the target.
Recipe import remains issue #75.

## Files and grammar

Configuration files use only the `.liquid` extension. Strings are UTF-8 and comments begin with
`#` or `//`. Commas and semicolons may separate fields; the formatter removes them where a newline
is unambiguous.

```ebnf
document       = { declaration } ;
declaration    = package | build | source | lock | generator | include ;
package        = "package", name, "{", package-item*, "}" ;
package-item   = field | variant | depends-on | provides | conflicts | patch | when ;
build          = "build", name, "{", build-item*, "}" ;
build-item     = field | depends-on | install | when ;
source         = "source", name, "{", field*, "}" ;
lock           = "lock", name, "{", field*, "}" ;
variant        = "variant", name, "{", field*, "}" ;
depends-on     = "depends_on", name, "{", field*, "}" ;
provides       = "provides", name, "{", field*, "}" ;
conflicts      = "conflicts", [ name ], "{", field*, "}" ;
patch          = "patch", [ name ], "{", field*, "}" ;
install        = "install", "{", ( field | install-item )*, "}" ;
install-item   = service | user | group | directory | symlink | cache | alternatives ;
service        = "service", "{", field*, "}" ;
user           = "user", "{", field*, "}" ;
group          = "group", "{", field*, "}" ;
directory      = "directory", "{", field*, "}" ;
symlink        = "symlink", "{", field*, "}" ;
cache          = "cache", name, "{", "}" ;
alternatives   = "alternatives", "{", field*, "}" ;
when           = "when", ( string | spec-text ), "{", package-item* | depends-on*, "}" ;
generator      = "generate", "=", "build", "{", field*, "}" ;
include        = "include", string ;
field          = identifier, ( ":" | "=" ), value ;
value          = string | integer | boolean | version | spec | digest | path | identifier
               | list | map ;
spec           = "spec", string ;
path           = "path", string ;
list           = "[", [ value, { ",", value } ], "]" ;
map            = "{", [ map-entry, { ",", map-entry } ], "}" ;
map-entry      = ( identifier | string ), ":", value ;
version        = digit, { digit | "." | "-" | "+" } ;
digest         = ( "blake3:" | "sha256:" | "blake3k:" ), 64 * hex-digit ;
name           = identifier | string ;
```

`when` accepts the public [`parse_spec`](resolution.md) grammar and is the only conditional. A
condition records its spec constraint on the declarations it contains; it cannot run code or read
ambient state. `include "relative/path.liquid"` resolves relative to the containing file, must
remain under the nearest liquid or Git repository root, must end in `.liquid`, and must not form a
cycle. URLs and absolute or parent-directory includes are rejected.

## Types and records

| Record | Required fields | Optional fields and children |
|---|---|---|
| `package <name>` | `versions: [version, ...]` | `compilers`, `targets`; `variant`, `depends_on`, `provides`, `conflicts`, `patch`, `when` |
| `variant <name>` | `values: [boolean|string, ...]`, `default: boolean|string` | none |
| `depends_on <name>` | none | `virtual: boolean`, `version: version|string`, `type: build|link|run` |
| `provides <name>` | none | `version` |
| `conflicts` | `constraint: spec` | `message: string` |
| `patch` | `digest: digest` | none |
| `source <name>` | `url: string`, `digest: digest`, `kind: archive|file` | none |
| `lock <name>` | `paths: [path, ...]` | none |
| `build <name>` | `command: [string, ...]` | `cwd`, `environment`, `sources`, `dependencies`, `outputs`, `inputs`, `toolchain`; `depends_on`, `when`, `install` |
| `install` | none | `layout: prefix`, `prefix: path`; `service`, `user`, `group`, `directory`, `symlink`, `cache`, `alternatives` |
| `service` | `name`, `exec`, `user`, `after`, `restart` | none |
| `user` | `name`, `system`, `home` | none |
| `group` | `name`, `system` | none |
| `directory` | `path`, `mode`, `owner` | none |
| `symlink` | `path`, `target` | none |
| `cache <name>` | one of `ldconfig`, `desktop-database`, `mime-database`, `man-db`, `icon-cache`, `systemd-daemon-reload`, `fonts` | none |
| `alternatives` | `name`, `path`, `priority` | none |
| `generate = build` | `command`, `output`, `inputs`, `toolchain` | `cwd`, `environment`, `outputs`, `format: liquid|cbor` |

Versions are deliberately unquoted, exact digests carry their algorithm, specs and paths use
explicit literal prefixes, and enum values are unquoted. Lists are homogeneous where the field
schema requires it. Environment maps have string values.

Every checker diagnostic has this exact two-line shape:

```text
file.liquid:7:14: expected version, found string '1.14.3'
  hint: versions are unquoted: 1.14.3
```

Unknown fields name the closest valid field. Duplicate fields and record names are rejected. The
portable test suite pins twenty complete diagnostics, including file, line, column, expectation,
actual value, and hint.

Install names are unquoted identifiers matching `[a-z_][a-z0-9_-]*`. Install paths use `path`
literals and must be canonical absolute paths containing only alphanumeric characters, `/`, `.`,
`_`, `+`, and `-`. Modes use octal digits from `0000` through `0777`. Restart is `no`,
`on-failure`, or `always`, and alternatives priorities must fit a signed 32-bit integer. These
checks run before lowering, so no arbitrary command text can enter an install intent or generated
maintainer script.

## Lowering and canonical records

`lower` maps `package` directly to the canonical `package/1` record from
[`resolve.hpp`](../../include/liquid/resolve.hpp). Conditions become its existing `when` strings.
A `build` becomes canonical `target/1`:

```text
target/1 {
  name, command, cwd, environment,
  sources: [declared digest ...],
  dependencies: [{name, when?} ...],
  outputs, install_layout, install_intent/1?, inputs?, toolchain?, origin
}
```

Dependency results enter an action below `deps/<name>` through the existing named-input mechanism.
Fetched source bytes enter the same immutable action-input mechanism below
`deps/__liquid_sources/<algorithm>/<digest>`, so network activity finishes before sandbox execution
and does not mutate the repository working tree. `source/1` is `{name, url, digest, kind, origin}`.
Both records use canonical CBOR; decoders reject a non-canonical form.

A `lock` becomes a `lock_rule` in `lowered_configuration::locks`. These repository-relative path
patterns are persisted by `liquid init`; [locks.md](locks.md) defines matching and authority.

An origin is either `source(file)` or `generated(file, result digest)`. Package origins are kept
beside `package/1` because changing that already-shipped record would change its identity. Target
and source records carry the origin directly.

The public surface is:

```cpp
result<configuration_document> parse_configuration(std::string_view, std::filesystem::path origin);
std::vector<configuration_diagnostic> check(const configuration_document&);
result<lowered_configuration> lower(const configuration_document&);
result<std::string> format_configuration(const configuration_document&);
runtime::task<lowered_configuration> run_generators(repository&, configuration_document);
runtime::task<result_id> build_configuration(repository&, const lowered_configuration&,
                                             std::string target);
```

Parsing, checking, lowering, and formatting are synchronous. Source fetch, generator execution,
and target builds are Capy tasks. None starts a raw thread, and no coroutine calls `block_on`.

## Sources by digest

`repository::fetch_source(source)` first reuses content already present under the declared digest.
Otherwise it performs a GET through liquid's existing HTTP client. HTTPS uses the existing Corosio
TLS path, and the operation refuses plaintext HTTP even when called without the checker. The
response is hashed with the declared unkeyed algorithm before any write reaches the store. A
mismatch returns both expected and actual digests and leaves the bytes absent. There is no overload
without a declared digest.

## Ordinary-program generator

```liquid
generate = build {
    command: ["bin/write-packages"]
    inputs: blake3:<input-tree>
    toolchain: blake3:<toolchain-tree>
    output: path "generated.liquid"
    outputs: [path "generated.liquid"]
    format: liquid
}
```

The command runs through `build::execute` with the fixed environment, sandbox, cache, result
signature, and `slsa_provenance/1` production used by every action. Its selected output must be a
`.liquid` document or a canonical CBOR array whose elements are byte strings containing
`package/1`, `target/1`, or `source/1`. Nested generators are refused. `run_generators` verifies
the result provenance before parsing its output and replaces every generated origin with that
result digest. `liquid config lower --trace` displays those origins.

## CLI

```sh
liquid config check <file|dir>
liquid config lower <file> [--json] [--trace]
liquid config format <file>
liquid build --target <name> [--json]
```

`config check` checks every `.liquid` file under a directory. `format` writes the canonical form;
formatting twice produces identical bytes. A repository build prefers `liquid.liquid`, then the
first sorted top-level `.liquid` file, runs generators, fetches every selected source, resolves
unconditional named target outputs, and executes the target through the ordinary action path.
Before returning the result, it applies the relocation publication check to every declared output.
Conditional target edges remain solver input and are not guessed by the direct command.

## Open questions

- Issue #75 owns recipe import and how imported files choose their top-level root document.
