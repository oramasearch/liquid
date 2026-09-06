# Structural editing

**What.** Liquid #205 provides C and C++ structural edits in libliquid for agents, the `liquid`
CLI, and liquify. Callers preview rename, extract, move, and concrete-tree changes, inspect exact
text replacements and refusals, then apply that immutable preview through the repository or its
daemon.

**Status.** The public records and canonical-CBOR codecs, repository and daemon tasks, four edit
operations, command-line client, grouped operation log, and operation undo are implemented. The
bundled tree-sitter frontend exposes concrete C and C++ syntax; the Clang frontend explicitly does
not. Languages beyond C and C++ remain liquid #72.

## Public records

[`liquid/edit.hpp`](../../include/liquid/edit.hpp) contains plain values shared with liquify. All
source ranges use zero-based, half-open byte offsets and one-based lines and columns.

```text
edit_node         = { kind, range, fingerprint }
edit_target       = { path, node }
tree_edit         = { target, operation, text }
edit_location     = symbol-id | { path, line, column }
rename_request    = { at, new_name }
extract_request   = { path, range, name, kind }
move_request      = { at, to_path, at_line? }
tree_edit_request = { edits }
edit_attribution  = { tool, operation, agent? }
text_change       = { range, old_text, new_text }
file_edit         = { path, before, after, changes }
edit_refusal      = { target, reason }
edit_preview      = { changes, refused, attribution, id }
edit_result       = { drips, operation }
```

The node fingerprint is BLAKE3 over the exact source bytes in its range. Request, preview, and
result codecs accept only canonical CBOR. Each top-level map has `v: 1` and a `kind` discriminator.
Nested records use the field names above; tree operations are `replace_node`, `insert_before`,
`insert_after`, and `delete_node`, while extraction kinds are `function` and `value`. A digest is
the map `{ alg, bytes }`.

The preview ID is BLAKE3 over the canonical preview map without its `id` field. Encoding and
decoding rederive it, so changing a file edit, refusal, or attribution invalidates the record.

## Preview and apply

```cpp
runtime::task<edit_preview> repository::preview_edit(edit_request, edit_attribution);
runtime::task<edit_result> repository::apply_edit(const edit_preview&);
runtime::task<edit_preview> daemon::preview_edit(edit_request, edit_attribution);
runtime::task<edit_result> daemon::apply_edit(edit_preview);
```

Preview reads working-tree blobs and their content-addressed symbol records through the
repository. It returns sorted per-file changes with only the affected ranges. Apply first checks
every `before` digest against the current tree and returns `errc::conflict` with the changed path
before writing anything if a file is stale. It applies the recorded replacements without planning
again, checks every resulting `after` digest, and writes each affected file through
`repository::write`. The bytes after apply are therefore identical to the bytes shown by preview.

Each file write produces one drip. The repository replaces the individual write-operation rows
with one `structural_edit` operation containing all before/after effects, the preview ID, and the
tool, operation, and optional agent attribution. `repository::undo` and `liquid undo` use that one
operation ID to restore every file.

The daemon carries `edit_preview` and `edit_apply` requests as canonical-CBOR edit records inside
the existing length-framed local protocol. The service opens the same repository graph used by
capture and build operations. Module work that can block remains on the runtime's compute pool.

## Operations

### Rename

Rename resolves the selected occurrence by symbol ID or source location. It changes only ranges in
stored symbol records: compiler-origin records are used when a recorded build supplied them, and
tree-sitter records remain explicitly `syntactic` fallback. The origin stays available on each
`symbol_record` used to create the preview.

Identifiers in comments and string or character literals are never edited. Matching occurrences
there are returned as `unresolved` refusals. Every C or C++ blob without a symbol record is returned
as `not indexed`; rename never searches its text.

A definition already named `new_name` refuses the complete request when it has the same container
as the selected symbol. Two free definitions collide only in the same file scope. The refusal
names the conflicting symbol ID, path, line, and column. No file change is returned for a collision.

### Extract

The selection must be inside one indexed definition. Extract-function finds identifiers used in
the selection but declared in the enclosing scope, resolves their C or C++ types, emits a new
`static void` definition immediately before the enclosing definition, and replaces the selection
with a call. Values that cannot be typed, and selections with an unresolved return type, are
refused instead of inferred.

Extract-value inserts an initialized `const auto` local at the selection line and replaces the
selected expression with its new name. The caller remains responsible for choosing an expression
range whose initialization is valid C or C++.

### Move

Move cuts the selected definition, or its enclosing definition unit, from its source path and
inserts it at a one-based destination line. Without `at_line`, insertion is at the end of the
destination. Source and destination are separate `file_edit` records and therefore produce two
drips. Include and declaration repair is deliberately not performed; callers inspect the preview
and resulting compiler diagnostics.

### Tree edits

Tree edits use node kind, range, and fingerprint values obtained from the frontend `syntax`
operation. Replace and delete target the node range; before and after insert at its respective
boundary. Each fingerprint is rechecked against current source bytes while previewing. A mismatch
refuses only that edit as `stale node fingerprint`; no nearby node is guessed.

## CLI

The commands are documented in [`../cli/README.md`](../cli/README.md). They attach to the checkout
daemon, preview by default, print the exact changes as a unified diff, and accept `--json` for the
stable preview projection. `--apply` sends the returned preview back without recomputing it.
`--agent <name>` fills the optional attribution. Any refusal exits with status 1 and is not applied.

## Own-tree measurement

`liquid-edit-measure` indexes C and C++ files below liquid's own `src/` with the bundled
tree-sitter frontend, selects the symbol with the most recorded references, and times one preview.
On an Apple M3 Max macOS host on 2026-09-05 during concurrent development:

```text
sysctl -n vm.loadavg
{ 100.63 82.60 61.70 }
structural rename: symbol=c++:value references=4924 indexed_files=137 files_touched=137 changes=4926 elapsed_ms=424
```

Run it from a configured build with:

```sh
cmake --build build-static --target liquid-edit-measure
sysctl -n vm.loadavg
build-static/tests/liquid-edit-measure . build-static/edit-measure-state
```

This is local development evidence, not a cross-host benchmark.

## Verification and limits

Tests compile before and after edits with the configured Clang. They cover a copy of
`src/store/faults.cpp`, a two-file move with a shared header, compiler-record rename across two
translation units, excluded string text, collision refusal, extract-function, two-drip move,
stale fingerprints, exact after digests, grouped undo, canonical records, daemon round trips, and
CLI JSON.

Current limits are semantic rename beyond recorded symbols, languages beyond C and C++, automatic
include or declaration repair during move, structural diff, and network rendering of the local
daemon protocol.
