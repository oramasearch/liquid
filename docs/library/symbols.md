# Symbol records

**What.** A frontend produces one canonical `symbols/1` record for each translation-unit source
blob. It is an ordinary content-addressed store object produced by a derived action.

**Record.** The stored map is `{ v:1, kind:"symbols", subject:digest, language:text,
origin:"compiler"|"syntactic", occurrences:[...] }`. Every occurrence has a stable language
symbol identifier, display name, kind, `definition` or `reference` role, zero-based byte and
one-based line/column half-open range, and optional container and documentation text. The source
blob digest is the subject. The Clang module marks records `compiler`; tree-sitter marks fallback
records `syntactic`, so consumers cannot treat syntax as semantic resolution.

`frontend_graph` registers the `symbols` query. Its canonical value is the record encoding; the
caller publishes it with `repository::store_symbols`, which associates the source digest with the
record. `repository::symbols(source_digest)` resolves that association and decodes the value.

## Recorded compile actions

Symbols are a query over recorded compile actions. `liquid build` and `liquid wrap` retain each
compile action's input tree, toolchain tree, command, and result manifest in the build graph.
`liquid symbols <path>` finds the most recent recorded compile action that read `<path>`. For a
header, it queries every recorded translation unit that read the header. For each translation unit,
the frontend describes a symbol action derived from that compile action: the same input and
toolchain trees, the compile command with its object-output options replaced by AST extraction
options, and the symbol record as its output contract.

The build-engine adapter executes that description through `liquid::build::execute`. The resulting
action is sandboxed, input-keyed, cacheable, replayable, and eligible for remote execution under
the same policy as the compile it mirrors. The frontend never starts a compiler process itself.
The graph recognizes the action description by its canonical `clang-symbols` kind and decodes the
resulting compiler stdout into the stored record.
If there is no matching recorded compile action, the command reports `no recorded compile action
for <path>; run liquid build first`; it never attempts a fresh compiler invocation.

The `symbols` query is separate from `frontend_graph::build`. Builds only demand compile and link
queries. An editor or `liquid symbols` demands symbol extraction at lower priority after a build
has recorded the compile action, so extraction is not on an edit's compile or relink path.

## Command line

`liquid symbols <path>` selects recorded C++ compile actions, requests their derived symbol
actions, records the result, and prints its origin, record digest, and occurrences. When no
compiler frontend is installed, it falls back to tree-sitter for the requested file. The first
version accepts C++ paths.

## How to verify

`Symbols.CompilerRecordResolvesAReferenceAcrossTranslationUnits` checks definitions, references,
a cross-translation-unit identifier match, and repository round trip.
`Symbols.TreeSitterFallbackLabelsSyntacticResults` checks the fallback label and definition.

## Limits

The v1 identifier is language-qualified name plus definition container. It does not yet encode
C++ overload signatures, macros, template substitutions, generated headers, or system-header
references. Documentation is optional pending compiler comment extraction.
