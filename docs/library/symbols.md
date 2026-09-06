# Symbol records

**What.** A frontend produces one canonical `symbols/1` record for each translation-unit source
blob. It is an ordinary content-addressed store object produced by a derived action.

**Record.** The stored map is `{ v:1, kind:"symbols", subject:digest, language:text,
origin:"compiler"|"syntactic", grammar?:text, occurrences:[...] }`. Every occurrence has a stable language
symbol identifier, display name, kind, `definition` or `reference` role, zero-based byte and
one-based line/column half-open range, and optional container and documentation text. The source
blob digest is the subject. The Clang module marks records `compiler`; tree-sitter marks fallback
records `syntactic`, so consumers cannot treat syntax as semantic resolution.
Syntactic records set `grammar` to the `sha256:` identity of the complete canonical grammar lock;
compiler records omit it. Decoding remains compatible with records written before this optional
field existed.

`frontend_graph` registers the `symbols` query. Its canonical value is the record encoding; the
caller publishes it with `repository::store_symbols`, which associates the source digest with the
record. `repository::symbols(source_digest)` resolves that association and decodes the value.

## Recorded compile actions

Symbols are a query over recorded compile actions. `liquid build` and `liquid wrap` retain each
compile action's input tree, toolchain tree, command, and result manifest in the build graph.
The command opens the recorded build graph's cache repository when it is present, rather than
creating a second association in the working repository.
`liquid symbols <path>` finds the most recent recorded compile action whose input tree includes
`<path>`. For a header, it queries every recorded translation unit whose input tree includes that
header. For each translation unit,
the frontend describes a symbol action derived from that compile action: the same input and
toolchain trees, the compile command with its object-output options replaced by AST extraction
options, and the symbol record as its output contract.

The build-engine adapter executes that description through `liquid::build::execute`. The resulting
action is sandboxed, input-keyed, cacheable, replayable, and eligible for remote execution under
the same policy as the compile it mirrors. The frontend never starts a compiler process itself.
The derived action declares the compile's complete toolchain tree: compiler, dynamic closure,
Clang resources, and required Linux driver-identification files. Its library search path is `/lib`,
the toolchain's read-only sandbox alias, and is part of the action key; no host library or system
configuration path is permitted.
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
compiler frontend supports the detected language, it falls back to the registered tree-sitter
grammar for the requested extension. This includes Python and every locked language whose upstream
supplies a pinned `tags.scm`; grammars without one return an explicit unsupported result for symbol
and unit discovery while retaining parse support.

## How to verify

`Symbols.CompilerRecordResolvesAReferenceAcrossTranslationUnits` checks definitions, references,
a cross-translation-unit identifier match, and repository round trip.
`Symbols.TreeSitterFallbackLabelsSyntacticResults` checks the fallback label, grammar identity, and
definition. `symbols-cli-python` checks that a Python file reports `origin=syntactic`.

## Limits

The v1 identifier is language-qualified name plus definition container. It does not yet encode
C++ overload signatures, macros, template substitutions, generated headers, or system-header
references. Documentation is optional pending compiler comment extraction.
