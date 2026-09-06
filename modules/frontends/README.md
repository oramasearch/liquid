# Frontend modules

Frontend implementations are separate ABI modules. `common/` contains the private canonical-CBOR
and tree-sitter adapters shared by the bundled implementations. `grammars/grammars.lock` is the
canonical input for the generated 49-language registry. `tree_sitter/` parses every configured
grammar and discovers syntax units from pinned tag queries without compiling. `clang/` emits C++ definition actions and assembles their object
outputs. The ABI and ownership rules are documented in
[`docs/library/frontends.md`](../../docs/library/frontends.md).

Neither module links into libliquid. CMake builds dynamic module targets for runtime loading and a
Clang static target for applications that register a descriptor at link time.

Both bundled modules implement the `symbols` ABI operation. Their origin label distinguishes the
compiler module from tree-sitter syntax fallback; the record is in
[`docs/library/symbols.md`](../../docs/library/symbols.md).
Grammar distribution, trust, subset/mirror controls, and maintenance are documented in
[`docs/library/grammars.md`](../../docs/library/grammars.md).
