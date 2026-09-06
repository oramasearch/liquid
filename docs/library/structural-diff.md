# Structural source diff

**Status: implemented by liquid #68.** Source files with a locked tree-sitter grammar can be
compared as syntax trees through the public repository API and `liquid diff --structural`.
Files without a grammar retain a text diff and carry an explicit reason for that fallback.

## One parse, three consumers

`frontend_graph` owns the `parse_tree` query. Its key is the language plus the BLAKE3 digest of
the source blob, and the digest-addressed overlay input contains the source bytes. Parsing runs
through `runtime::offload`; callers do not create parser threads or synchronously wait inside a
coroutine.

The frontend ABI's optional `parse` tail returns canonical CBOR containing:

- the language, grammar-lock digest, and SHA-256 identity of the grammar's `node-types.json`;
- a compact node-kind table and numeric kind IDs;
- zero-based byte ranges, one-based line and column ranges, named children, field names, and
  error or missing flags; and
- unit and symbol sidecars derived while that tree is live.

The build-unit and symbol operations consume those sidecars from the graph-owned value. Structural
diff consumes the decoded tree. A build followed by a diff therefore executes the parser once for
the unchanged blob; only the new blob adds a parse.

## Matching and edit script

Comparison uses a GumTree-style sequence:

1. Declaration and definition fingerprints anchor unchanged units without their absolute ranges.
2. Unique semantic-subtree hashes add top-down anchors.
3. Matched descendants select compatible containers bottom-up.
4. Unmatched children of matched containers pair by kind and field.
5. Longest-common-subsequence order checks distinguish moves from insertions and removals.

Subtree hashes omit whitespace and comment trivia. Reindentation, line wrapping, and brace
placement therefore produce no edits and set `formatting_only`. Comments are compared in a
separate pass and produce `comment_changed`; they never disappear behind trivia filtering. The
stable edit kinds are `added`, `removed`, `modified`, `moved`, and `comment_changed`.

## Public result and fallback tiers

`tree_diff_options::structural` requests the comparison and
`tree_diff_options::frontend` supplies the graph. Each `tree_diff_entry` reports a `diff_tier` of
`structural`, `text`, or `binary`, plus `tier_reason` when a lower tier was selected. The optional
`structural_diff` value contains the language, grammar-lock digest, formatting flag, and ordered
edits. Each edit contains its kind, node kind, optional grammar-derived name, and optional old and
new ranges.

An unknown extension is returned as text with a reason such as `no grammar for .abc`. The API does
not silently present that result as a structural comparison. Added and removed source files parse
against an empty opposite source so their syntax additions or removals remain structured.

## CLI

`liquid diff <a> <b> [path] --structural` prints concise records such as:

```text
moved function parse_header: 10-30 -> 45-65
reformat only source.cpp
text (no grammar for .abc) notes.abc
```

With `--json`, every selected entry includes the shared support `tier`, structural `diff_tier`,
`tier_reason`, and either a `structural` object or `null`. Structural objects contain `language`,
`grammar`, `formatting_only`, and an ordered `edits` array. Ranges contain byte, line, and column
positions.

## Verification and measurement

The structural test matrix contains handwritten move, formatting, and semantic-edit pairs for C,
C++, Python, JavaScript, TypeScript, Go, Rust, Java, Ruby, and Shell. Every other grammar in the
lock mechanically reindents its committed sample and must return an empty formatting-only edit
script. Separate tests cover comment changes, fallback labels, CLI text and JSON, ABI tail
compatibility, and one parse per blob across a build followed by a diff.

The own-source measurement imported `origin/main` and a second tree containing this change's 16
production source files, then ran a cold-cache comparison with:

```text
liquid diff before after --structural
```

On the macOS development host, with load average `{ 19.31 37.17 50.51 }` before the run, the
comparison took 41.22 seconds real time, 40.50 seconds user time, and 0.45 seconds system time. It
returned 16 structural entries containing 229 edits. The graph directory was removed immediately
before the run so the measurement includes parsing both states.
