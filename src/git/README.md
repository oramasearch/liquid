# Git interoperability implementation

`object_store.cpp` reads and verifies loose and packed Git objects. All object reads share
bounded inflation and delta reconstruction, including REF_DELTA bases in other packs and
loose storage. Offset bases must identify a strictly earlier indexed object. Delta chains
resolve iteratively, so their length never reaches the native stack. The byte and depth
limits apply to the CLI and library importer through this reader.

`import.cpp` maps reachable Git history into liquid records; `export.cpp` writes standalone
Git repositories. See [Git interoperability](../../docs/library/git-interop.md) for the
mapping, resource limits, and compatibility rules.
