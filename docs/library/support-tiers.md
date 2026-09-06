# Support-tier matrix

**Status: implemented by liquid #71.** libliquid publishes the language, format, and platform
support declarations used by the CLI and generated feature tables.

## Public API

[`tiers.hpp`](../../include/liquid/tiers.hpp) defines the plain `language_support`,
`format_support`, `platform_support`, and `matrix` values. `support_matrix()` is synchronous pure
computation over `frontend_registry::global()` and `format_registry::global()` at the instant of
the call. It performs no runtime I/O and keeps no second language or codec inventory.

The language matrix always includes the `*` Tier 0 row because chunk storage and versioning do not
need a language registration. Loaded grammar descriptors become Tier 2 rows with
`grammar(<lock-digest>)`; loaded compiler modules become Tier 3 rows with
`frontend(<module-name>)`. When a compiler frontend and grammar register the same language, the
registry unions their extensions and reports the stronger compiler frontend. The grammar remains
available as its fallback module.

The format matrix always includes the `unknown` Tier 3 row. Every other row is projected from a
live `format_descriptor`, including its advertised best fidelity tier, codec name, and exact depth,
decoded-byte, and entry limits. Recognition may still lower an individual object below that best
tier, as described in [formats.md](formats.md).

Platform rows describe the wrapper contract rather than the machine running the query. Linux uses
namespaces with FUSE, macOS uses `sandbox-exec` with macFUSE, and Windows and BSD currently have no
Tier 1 wrapper backend. Keeping all platform rows in every build makes the generated documentation
identical across static, shared, Linux, and macOS verification.

## Frontend registration

`frontend_registry::global()` is the process registry consumed by `support_matrix()`. A client
loads its installed frontend modules before querying. Multiple modules may register the same
language; duplicate module names are rejected, `find()` selects a compiler frontend over a grammar,
and `languages()` returns one sorted row per language.

The `liquid tiers` client loads the installed tree-sitter and Clang modules before calling the
matrix API. A build without bundled frontends still reports Tier 0, registered formats, and all
platform declarations.

## Verification

`SupportMatrix.ContainsEveryRegisteredCodec` compares the matrix with every codec in the live
format registry. When bundled frontends are enabled,
`SupportMatrix.ContainsEveryLockedGrammarAndPrefersCompilerFrontends` compares the language rows
with the generated grammar registry and verifies that C++ resolves to the Clang Tier 3 module.

## Limits

The matrix reports declared maximum support, not a guarantee that an arbitrary input is valid for
that language or format. Per-object recognition and command output report the tier actually used.
