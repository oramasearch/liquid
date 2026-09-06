# Frontend integration

This directory implements libliquid's checked frontend-module loader and the query-graph
coordinator. Language-specific parsing and compilation do not belong here; they live in separate
targets under [`modules/frontends/`](../../modules/frontends/). The public contract and canonical
values are documented in [`docs/library/frontends.md`](../../docs/library/frontends.md).
The loader also validates the optional ABI-v1 language metadata tail. The public registry projects
language, extensions, frontend-or-grammar mechanism, and grammar-lock digest without changing the
existing module language-name method.

`symbols.cpp` validates and encodes the frontend graph's `symbols` query value. Its public record
contract is in [`docs/library/symbols.md`](../../docs/library/symbols.md).
`syntax.cpp` validates the optional ABI v1 concrete-tree record and provides parent-linked
`node_at` and `enclosing` navigation without exposing a parser type.
Syntactic records preserve the optional grammar-lock identity through canonical encoding and
decoding.
It also validates the compact canonical parse record. `graph.cpp` owns the `parse_tree` query keyed
by language and blob digest, passes that value to unit and symbol operations, and exposes path-to-
grammar selection for structural diff.
