# Symbol records

**What.** A frontend produces one canonical `symbols/1` record for each translation-unit source
blob. It is an ordinary content-addressed store object and an action sidecar.

**Record.** The stored map is `{ v:1, kind:"symbols", subject:digest, language:text,
origin:"compiler"|"syntactic", occurrences:[...] }`. Every occurrence has a stable language
symbol identifier, display name, kind, `definition` or `reference` role, zero-based byte and
one-based line/column half-open range, and optional container and documentation text. The source
blob digest is the subject. The Clang module marks records `compiler`; tree-sitter marks fallback
records `syntactic`, so consumers cannot treat syntax as semantic resolution.

`frontend_graph` registers the `symbols` query. Its canonical value is the record encoding; the
caller publishes it with `repository::store_symbols`, which associates the source digest with the
record. `repository::symbols(source_digest)` resolves that association and decodes the value.

## Command line

`liquid symbols <path>` reads the repository blob, prefers the installed Clang module, falls back
to tree-sitter, records the result, and prints its origin, record digest, and occurrences. The
first version accepts C++ paths.

## How to verify

`Symbols.CompilerRecordResolvesAReferenceAcrossTranslationUnits` checks definitions, references,
a cross-translation-unit identifier match, and repository round trip.
`Symbols.TreeSitterFallbackLabelsSyntacticResults` checks the fallback label and definition.

## Limits

The v1 identifier is language-qualified name plus definition container. It does not yet encode
C++ overload signatures, macros, template substitutions, generated headers, or system-header
references. Documentation is optional pending compiler comment extraction.
