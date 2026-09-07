# Frontend modules

Frontend implementations are separate ABI modules. `common/` contains the private canonical-CBOR
and tree-sitter adapters shared by the bundled implementations. `grammars/grammars.lock` is the
canonical input for the generated 49-language registry. `tree_sitter/` parses every configured
grammar and discovers syntax units from pinned tag queries without compiling. `clang/` emits C++ definition actions and assembles their object
outputs. The ABI and ownership rules are documented in
[`docs/library/frontends.md`](../../docs/library/frontends.md).

The installed modules remain dynamic runtime extensions. CMake also compiles the tree-sitter module
descriptor into libliquid for convergence-time structural merge, while applications may register
the Clang static target explicitly.

Both bundled modules implement the `symbols` ABI operation. Their origin label distinguishes the
compiler module from tree-sitter syntax fallback; the record is in
[`docs/library/symbols.md`](../../docs/library/symbols.md).
The optional ABI v1 `syntax` operation returns the complete concrete tree as canonical CBOR.
Tree-sitter implements it for every configured grammar; Clang leaves its descriptor pointer null.
Shared encoding and traversal live in `common/syntax.cpp`.
The later optional `parse` tail returns a compact named-node tree plus unit and symbol sidecars.
Both bundled modules implement it, and their unit and symbol operations consume the supplied parse
sidecar so graph consumers do not invoke the parser again.
Grammar distribution, trust, subset/mirror controls, and maintenance are documented in
[`docs/library/grammars.md`](../../docs/library/grammars.md).
